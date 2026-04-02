# GPU Codegen — From `kernel {}` to CUDA

## What This Is

When a user writes `kernel double_arr(a: [3]f64) -> [3]f64 { map(a, ...) }` and
compiles with `--gpu=cuda`, the compiler:

1. Generates the same `linalg.map` on tensors as the CPU path
2. Wraps it in GPU memory management (alloc device memory, copy data over, run kernel, copy back)
3. Lowers through MLIR's GPU dialect pipeline to actual PTX/cubin
4. Embeds the compiled GPU binary in the executable
5. Links against CUDA runtime wrappers

The user doesn't write any GPU-specific code. The `kernel {}` syntax is the same
for both CPU and GPU — `--gpu=cuda` just changes where it runs.

## How It Was Built (3 Phases)

### Phase 1: Plumbing

**Commits:** `6f6c38a`, `c3d6ac8`

The first step was wiring up the infrastructure without changing any behavior.

- Added `--gpu={cuda,amd}` flag via CLI11. The value flows through
  `compiler_option_enum::GPU` → `mlirGen()` → `MLIRGenImpl::gpuTarget` →
  `lowerMLIRToLLVMIR(gpuTarget)`.
- Rebuilt LLVM with `NVPTX` in `LLVM_TARGETS_TO_BUILD`. This is the only LLVM
  build change needed — `MLIR_ENABLE_CUDA_RUNNER` is only needed later for the
  runtime library, not for the compiler itself.
- Linked the GPU-related MLIR CMake targets: `MLIRGPUDialect`, `MLIRGPUTransforms`,
  `MLIRNVVMDialect`, `MLIRGPUToNVVMTransforms`, `MLIRSCFToGPU`, `MLIRNVVMToLLVM`,
  `MLIRUBToLLVM`, `MLIRIndexToLLVM`, `MLIRMathToLLVM`, `MLIRComplexToLLVM`,
  `MLIRVectorToLLVM`, `MLIRNVVMTarget`, `MLIRRegisterAllExtensions`,
  `MLIRRegisterAllDialects`.
- Conditionally register `gpu::GPUDialect` when `--gpu` is set.

At this point, `--gpu=cuda` was accepted but did nothing different from the CPU path.
All existing tests continued to pass.

### Phase 2: Data Marshalling

**Commits:** `8b8891a`, `747b035`, `c46fa02`

This was the hardest phase. The challenge: GPU kernels operate on device memory,
but the host program has data in regular (host) memory. Someone needs to copy data
to the GPU before the kernel runs, and copy results back after.

**The B2 wrapper pattern.** Every `kernel` definition emits two functions:
- `@__kernel_double_arr` — the actual computation (linalg.map on tensors)
- `@double_arr` — the public wrapper that callers see

On the CPU path, the wrapper just wraps host pointers as memrefs and calls the
internal function. On the GPU path, the wrapper additionally:
1. Allocates device memory (`gpu.alloc`)
2. Copies host data to device (`gpu.memcpy`)
3. Calls the kernel with device memrefs
4. Copies results back (`gpu.memcpy`)
5. Frees device memory (`gpu.dealloc`)

**The refactor we had to do.** The initial approach put `gpu.alloc`/`gpu.memcpy`
directly in the existing wrapper, which mixed LLVM dialect ops (from
`buildMemrefFromPtr`) with GPU dialect ops. This broke `gpu-to-llvm` — that pass
does a single `applyPartialConversion` and can't handle pre-existing LLVM ops
alongside unconverted GPU ops in the same function.

The fix: the GPU wrapper takes `memref<NxT>` args instead of `!llvm.ptr`. The
wrapper body is pure `func`/`memref`/`gpu` dialect — no LLVM ops at all. The
conversion from `!llvm.ptr` to memref happens at the **call site** (in `main()` or
wherever the kernel is called), using `buildMemrefFromPtr`. This keeps the LLVM ops
in the CPU-side code where they belong, and the GPU wrapper is clean for `gpu-to-llvm`.

Key files:
- `MLIRGenImpl.h`: added `gpuTarget` member, `targetGPU()`, `gpuCopyToDevice`,
  `gpuCopyToHostAndDealloc`, `gpuDealloc`, `buildGpuWrapperFuncType`
- `MLIRGen.cpp`: GPU branch in `emitKernelWrapper`, forward-declaration uses
  `buildGpuWrapperFuncType` for GPU
- `MLIRGenFunction.cpp`: call site converts `!llvm.ptr` → memref when calling
  a kernel wrapper with `--gpu` (detected via `sammine.kernel_wrapper` attribute)

**The async discovery.** After all the refactoring, `gpu-to-llvm` still wasn't
converting `gpu.alloc`/`gpu.memcpy`/`gpu.dealloc`. Debugging showed that
`mlir-opt --gpu-to-llvm` also failed on the same IR. The root cause:
`gpu-to-llvm` only converts **async** GPU ops.

In `GPUToLLVMConversion.cpp`, `ConvertAllocOpToGpuRuntimeCallPattern::matchAndRewrite`
has this check:

```cpp
if (!isShared && failed(isAsyncWithOneDependency(rewriter, allocOp)))
    return failure();  // silently skip non-async alloc
```

The fix: emit async GPU ops with explicit `gpu.wait` synchronization barriers.
The execution is still synchronous (we wait after every operation), but the ops
satisfy the pattern matcher:

```mlir
%t0 = gpu.wait async
%dev, %t1 = gpu.alloc async [%t0] : memref<3xf64>
%t2 = gpu.memcpy async [%t1] %dev, %host : memref<3xf64>, memref<3xf64>
gpu.wait [%t2]            // block until copy completes
```

This is not documented anywhere in MLIR. The only way to discover it was reading
`GPUToLLVMConversion.cpp` line 773.

### Phase 3: Lowering Pipeline + Linking

**Commits:** `a234f9b`, `d3661e7`, `14c77b3`

With marshalling working, the lowering pipeline connects everything end-to-end.

**Kernel module pipeline (GPU path in `MLIRLowering.cpp`):**

```
bufferize (same as CPU — tensor → memref)
→ linalg-to-parallel-loops     (scf.parallel instead of scf.for)
→ gpu-map-parallel-loops        (annotate parallel dims as GPU blocks/threads)
→ convert-parallel-loop-to-gpu  (scf.parallel → gpu.launch)
→ gpu-kernel-outlining           (extract gpu.launch body → gpu.func in gpu.module)
→ gpu-nvvm-attach-target         (tag gpu.module with nvptx64-nvidia-cuda, sm_75)
→ gpu-to-nvvm                    (inside gpu.module: gpu.thread_id → nvvm intrinsics)
→ lower-affine                   (inside gpu.module: affine.apply → arith)
→ arith-to-llvm                  (inside gpu.module)
→ index-to-llvm                  (inside gpu.module)
→ reconcile-unrealized-casts     (inside gpu.module)
→ gpu-module-to-binary           (ptxas compiles gpu.module to cubin, embedded as blob)
→ expand-strided-metadata
→ lower-affine
```

**Merged CPU module pipeline (Phase 2):**

```
scf-to-cf
→ gpu-to-llvm     (replaces func-to-llvm + memref-to-llvm for GPU path)
→ reconcile-unrealized-casts
```

`gpu-to-llvm` is a `ConvertToLLVM`-style pass that handles all dialect-to-LLVM
conversion in one shot (func, memref, arith, cf, gpu). You can NOT run
`func-to-llvm` or `memref-to-llvm` before it — they'd convert types that the GPU
patterns still need to match.

**Module merge (Step C).** After the kernel pipeline, the kernel module's functions
and GPU binaries are moved into the CPU module:
- `func::FuncOp` → erase CPU module's forward declaration, move definition
- `gpu::BinaryOp` → move (serialized cubin blob)
- Set `gpu.container_module` attribute on CPU module (required by `gpu.launch_func`)

**Interface registration.** This was a major pain point. `gpu-to-llvm` collects
conversion patterns from all loaded dialects via `ConvertToLLVMPatternInterface`.
These interfaces are external models registered on the dialect registry. They MUST
be registered before the dialect is loaded — otherwise the interface isn't available
on the dialect instance.

```cpp
// In Compiler.cpp, BEFORE any getOrLoadDialect calls:
mlir::DialectRegistry earlyRegistry;
mlir::registerAllExtensions(earlyRegistry);
mlir::registerConvertToLLVMDependentDialectLoading(earlyRegistry);
mlir::gpu::registerOffloadingLLVMTranslationInterfaceExternalModels(earlyRegistry);
mlirCtx.appendDialectRegistry(earlyRegistry);
```

The `registerOffloadingLLVMTranslationInterfaceExternalModels` one is particularly
sneaky — without it, `gpu.binary` ops hit `assert(offloadingHandler)` during LLVM
IR translation.

**cuModuleUnload teardown.** MLIR's `SelectObjectAttr` implementation registers
GPU module load/unload as global constructors/destructors (priority 123). At
program exit, the global destructor calls `cuModuleUnload`, but by then the CUDA
driver has already deinitialized → error message.

Fix in `MLIRLowering.cpp`: after LLVM IR translation, strip `llvm.global_dtors`
and insert explicit calls to the unload functions before each `ret` instruction
in `main()`. This ensures `cuModuleUnload` runs while the CUDA context is alive.

**Linking.** GPU executables link against `libmlir_cuda_runtime.so`, which provides
the `mgpu*` wrapper functions (`mgpuMemAlloc` → `cuMemAlloc`, `mgpuMemcpy` →
`cuMemcpyAsync`, etc.). This library is built by setting `MLIR_ENABLE_CUDA_RUNNER=ON`
when building LLVM. The library path is derived from `MLIR_DIR` at cmake configure
time via a `MLIR_LLVM_LIB_DIR` compile definition.

## Testing

- `lit.cfg.py` detects `ptxas` on `$PATH` → adds `cuda` feature
- GPU tests use `REQUIRES: cuda` so they only run on machines with CUDA toolkit
- `kernel_map_gpu.mn` — compiles and runs `map` on GPU (verified on RTX 3070)
- `kernel_reduce_gpu.mn` — XFAIL, `linalg.reduce` GPU codegen segfaults at runtime
- `kernel_gpu_marshalling.mn` — golden test for MLIR output (checks gpu.alloc/memcpy/dealloc appear)

## Known Issues

- **`linalg.reduce` on GPU crashes.** The parallel reduction pattern from
  `linalg-to-parallel-loops` doesn't produce correct GPU code. Needs investigation.
- **`--gpu=amd` not implemented.** The flag is accepted but no ROCm lowering exists.
  Would need `gpu-to-rocdl` instead of `gpu-to-nvvm`, and `libmlir_rocm_runtime`.
- **No tiling.** Currently 1 GPU thread per array element. For large arrays, this
  creates too many threads. A tiling pass before `linalg-to-parallel-loops` would
  fix this.
- **Kernel composition not supported.** `map(double_arr(a), ...)` doesn't parse —
  `map`/`reduce` only accept variable names as array arguments.
- **No upstream issue for cuModuleUnload.** The teardown problem affects any MLIR
  AOT GPU compilation, but there's no tracking issue in llvm/llvm-project.

## File Reference

| File | GPU-related changes |
|------|-------------------|
| `src/sammine.cpp` | `--gpu={cuda,amd}` flag |
| `include/compiler/Compiler.h` | `GPU` enum value |
| `src/compiler/Compiler.cpp` | Early registry registration, GPU dialect loading, GPU link flags |
| `CMakeLists.txt` | `MLIR_LLVM_LIB_DIR` define |
| `src/codegen/CMakeLists.txt` | GPU CMake targets |
| `include/codegen/MLIRGen.h` | `gpuTarget` parameter on `mlirGen()` |
| `include/codegen/MLIRGenImpl.h` | `gpuTarget` member, `targetGPU()`, GPU helpers |
| `src/codegen/MLIRGen.cpp` | GPU wrapper codegen, `buildGpuWrapperFuncType`, async gpu helpers |
| `src/codegen/MLIRGenFunction.cpp` | Call site `!llvm.ptr` → memref conversion for GPU |
| `include/codegen/MLIRLowering.h` | `gpuTarget` parameter |
| `src/codegen/MLIRLowering.cpp` | GPU kernel pipeline, Phase 2 gpu-to-llvm, merge, teardown fix |
| `e2e-tests/lit.cfg.py` | `cuda` feature detection |
| `e2e-tests/compilables/kernel/kernel_map_gpu.mn` | GPU map e2e test |
| `e2e-tests/compilables/kernel/kernel_reduce_gpu.mn` | GPU reduce e2e test (XFAIL) |
| `e2e-tests/compilables/kernel/kernel_gpu_marshalling.mn` | MLIR output golden test |
