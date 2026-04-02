# Plan: CUDA Codegen from Kernel IR (Phase 7c)

## Goal

Lower existing `linalg.map`/`linalg.reduce` kernel IR to CUDA GPU code via MLIR's GPU dialect pipeline, with automatic host↔device data marshalling in the B2 wrapper functions.

## Prerequisites

- Phase 7a kernel linalg codegen: **DONE** (map/reduce emit to linalg on tensors, B2 wrapper pattern working)
- LLVM rebuilt with NVPTX target: **TODO**
- CUDA toolkit installed on host: **TODO**

## Current State

The kernel pipeline today (CPU path):

```
kernel double_arr(a: [3]f64) -> [3]f64 { map(a, (x: f64) -> f64 { x * 2.0 }) }

Emits two functions:
  1. @__kernel_double_arr(tensor<3xf64>) -> tensor<3xf64>   [linalg.map, tensor types]
  2. @double_arr(!llvm.ptr, !llvm.ptr)                       [CPU-ABI wrapper, sret]

Lowering:
  kernelModule: bufferize → linalg-to-loops → buffer opts → expand-strided-metadata → lower-affine
  merge into cpuModule
  cpuModule: scf→cf → arith→llvm → cf→llvm → memref→llvm → func→llvm → reconcile-casts
```

The GPU path forks at `linalg-to-loops` — instead of sequential loops, we produce parallel loops mapped to GPU threads.

---

## Architecture

### Two phases: codegen (MLIRGen) + lowering (MLIRLowering)

**Phase A: Codegen (MLIRGen.cpp) — GPU-aware wrapper emission**

When `--gpu=cuda`, `emitKernelWrapper` emits `gpu.alloc`/`gpu.memcpy`/`gpu.dealloc`
in the wrapper function. The `__kernel_*` internal function is unchanged (still
linalg on tensors). Without `--gpu`, the existing CPU wrapper is emitted.

**Phase B: Lowering (MLIRLowering.cpp) — pass pipeline fork**

```
if (targetGPU) {
  kernelModule:
    bufferize (same as today)
    → linalg → parallel loops (scf.parallel instead of scf.for)
    → map parallel loops to GPU grid dimensions
    → scf.parallel → gpu.launch
    → gpu-kernel-outlining (extracts device code into gpu.module/gpu.func)
    → nvvm-attach-target (tag gpu.module with NVPTX triple/chip)
    → convert-gpu-to-nvvm (gpu ops → NVVM dialect inside gpu.module)
    → gpu-module-to-binary (ptxas compiles to cubin, embedded as constant)
    → gpu-to-llvm (gpu.alloc→cudaMalloc, gpu.memcpy→cudaMemcpy, gpu.launch_func→cudaLaunchKernel)
    → merge into cpuModule
  cpuModule: existing pipeline (unchanged)
} else {
  existing CPU path (unchanged)
}
```

### Data marshalling

The lowering passes (`gpu-kernel-outlining`, `gpu-to-llvm`, etc.) do NOT automatically
insert device memory allocation or host↔device copies. They only transform the IR
structure. If we pass host memrefs to `gpu.launch_func`, the GPU kernel will try to
dereference host pointers → segfault.

**Solution: explicit marshalling in the B2 wrapper (GPU-aware codegen in MLIRGen.cpp).**

The wrapper is only GPU-aware when `--gpu` is passed. Without `--gpu`, the existing
CPU wrapper is emitted unchanged.

The B2 wrapper function `@double_arr` currently does (CPU path):

```
1. receives !llvm.ptr (pointing to caller's array)
2. builds memref from ptr (host memref)
3. calls @__kernel_double_arr(host memref) → result
4. returns (sret or scalar)
```

With `--gpu`, the wrapper instead does:

```
1. receives !llvm.ptr (pointing to caller's array on host)
2. builds host memref from ptr
3. gpu.alloc → device memref (for each array arg)
4. gpu.memcpy host_memref → device_memref (for each array arg)
5. gpu.alloc → device output memref (if returns array, for DPS output)
6. calls @__kernel_double_arr(device_memrefs..., device_output)
7. gpu.memcpy device_output → host sret memref (if returns array)
8. gpu.dealloc device memrefs
9. returns
```

The `gpu.alloc`/`gpu.memcpy`/`gpu.dealloc` ops are emitted directly in codegen.
The lowering passes (`gpu-to-llvm`) then convert them to `cudaMalloc`/`cudaMemcpy`/
`cudaFree` calls.

---

## Implementation Steps

### Step 0: Rebuild LLVM with NVPTX

**Not a code change — infrastructure prerequisite.**

```bash
cmake -S externals/llvm-project/llvm -B externals/llvm-project/build -G Ninja \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64;NVPTX" \
  -DLLVM_ENABLE_PROJECTS="mlir;llvm" \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
```

Key addition vs current build:
- `NVPTX` in `LLVM_TARGETS_TO_BUILD` — enables the NVPTX backend for PTX code generation

**What you do NOT need:**
- `MLIR_ENABLE_CUDA_RUNNER` — this builds `mlir_cuda_runtime`, a shared library for
  MLIR's JIT runner (`mlir-cpu-runner --shared-libs=libmlir_cuda_runtime.so`). We don't
  use the JIT runner; we compile to object files and link against `-lcudart` directly.
- `MLIR_ENABLE_NVPTXCOMPILER` — this statically links NVIDIA's `nvptxcompiler` library
  for PTX→cubin compilation. Without it, MLIR shells out to `ptxas` (from CUDA toolkit)
  as a subprocess, which works fine.

**System requirements:**
- CUDA toolkit installed (provides `ptxas` for PTX→cubin, `libcudart` for linking)
- `ptxas` must be on `$PATH` (or set via pass options)

**Verification:** `llc --version | grep NVPTX` should list the NVPTX target.

---

### Step 1: CMake + GPU Dialect Registration

**File: `src/codegen/CMakeLists.txt`**

Add GPU-related MLIR libraries:

```cmake
# GPU dialects
MLIRGPUDialect
MLIRGPUTransforms              # gpu-kernel-outlining, gpu-map-parallel-loops
MLIRNVVMDialect                # NVVM IR ops

# GPU conversions
MLIRGPUToNVVMTransforms        # convert-gpu-to-nvvm
MLIRGPUToLLVMIRTranslation     # gpu-to-llvm (host launch stubs)
MLIRSCFToGPU                   # convert-parallel-loops-to-gpu (via scf.parallel)
MLIRNVVMToLLVM                 # convert-nvvm-to-llvm
```

**File: `src/compiler/Compiler.cpp`**

In `codegen_mlir()`, conditionally register GPU dialects when `--gpu` flag is set:

```cpp
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/NVGPU/IR/NVGPUDialect.h"  // if needed

// Inside codegen_mlir(), alongside existing kernel dialect registration:
if (options.gpu) {
  mlirCtx.getOrLoadDialect<mlir::gpu::GPUDialect>();
}
```

**File: `src/sammine.cpp`**

Add `--gpu` CLI flag (via CLI11, same pattern as `--backend`).

**File: `include/compiler/Compiler.h`**

Add `GPU` to `compiler_option_enum` or add a `bool gpu` to compiler options.

**Verification:** `cmake --build build -j` compiles. All existing tests pass (GPU dialects loaded but unused).

---

### Step 2: GPU-Aware Kernel Wrapper (marshalling)

**Files: `include/codegen/MLIRGenImpl.h`, `src/codegen/MLIRGen.cpp`**

The `--gpu` flag must be threaded into MLIRGenImpl so `emitKernelWrapper` can
emit GPU marshalling ops when targeting GPU. Without `--gpu`, the existing CPU
wrapper is emitted unchanged.

**MLIRGenImpl changes:**
- Add `bool targetGPU` member, set from constructor/caller
- `mlirGen()` signature gets a `gpuTarget` parameter, passed from `Compiler.cpp`

**emitKernelWrapper changes (GPU path):**

When `targetGPU` is true, the wrapper emits explicit device memory management
instead of passing host memrefs directly to the kernel:

```cpp
void MLIRGenImpl::emitKernelWrapper(AST::KernelDefAST *kd, ...) {
  // ... existing preamble (get wrapperOp, create entry block) ...

  if (targetGPU) {
    // === GPU wrapper: marshal host → device → kernel → host ===
    llvm::SmallVector<mlir::Value> deviceMemrefs;  // to dealloc later
    llvm::SmallVector<mlir::Value> kernelArgs;

    for (each array param) {
      auto hostMemref = buildMemrefFromPtr(blockArg, ...);
      auto memrefType = hostMemref.getType().cast<mlir::MemRefType>();

      // gpu.alloc device memory
      auto deviceMem = gpu::AllocOp::create(builder, loc,
          memrefType, /*asyncToken=*/nullptr, /*asyncDeps=*/{},
          /*dynamicSizes=*/{}, /*symbolOperands=*/{});

      // gpu.memcpy host → device
      gpu::MemcpyOp::create(builder, loc, /*asyncToken=*/nullptr,
          /*asyncDeps=*/{}, deviceMem.getMemref(), hostMemref);

      kernelArgs.push_back(deviceMem.getMemref());
      deviceMemrefs.push_back(deviceMem.getMemref());
    }

    // DPS output: allocate device output buffer
    if (returnsArray) {
      auto sretPtr = entryBlock.getArgument(entryBlock.getNumArguments() - 1);
      auto hostOutMemref = buildMemrefFromPtr(sretPtr, ...);
      auto deviceOut = gpu::AllocOp::create(builder, loc, ...);
      kernelArgs.push_back(deviceOut.getMemref());

      // Call kernel with device args
      func::CallOp::create(builder, loc, internalName, ..., kernelArgs);

      // gpu.memcpy device output → host sret
      gpu::MemcpyOp::create(builder, loc, ..., hostOutMemref, deviceOut.getMemref());

      // gpu.dealloc all device buffers
      gpu::DeallocOp::create(builder, loc, ..., deviceOut.getMemref());
    } else {
      // scalar return: call kernel, dealloc inputs
      auto call = func::CallOp::create(builder, loc, internalName, ..., kernelArgs);
    }

    for (auto dm : deviceMemrefs)
      gpu::DeallocOp::create(builder, loc, ..., dm);

    // return
    func::ReturnOp::create(builder, loc, ...);

  } else {
    // === CPU wrapper: existing code unchanged ===
    // ... current emitKernelWrapper body ...
  }
}
```

**Key detail:** The `gpu.alloc`/`gpu.memcpy`/`gpu.dealloc` ops are emitted into the
**kernel module** (since the wrapper lives there alongside `__kernel_*`). The lowering
passes in Step 3 then convert them: `gpu-to-llvm` turns them into `cudaMalloc`/
`cudaMemcpy`/`cudaFree` calls.

**Verification:** With `--mlir-ir --gpu=cuda`, a kernel test should show `gpu.alloc`,
`gpu.memcpy`, `gpu.dealloc` ops in the wrapper function.

---

### Step 3: GPU Lowering Pipeline

**File: `src/codegen/MLIRLowering.cpp`**

New includes:

```cpp
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "mlir/Conversion/SCFToGPU/SCFToGPUPass.h"
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"
#include "mlir/Conversion/GPUCommon/GPUCommonPass.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
```

Change function signature to accept a `targetGPU` flag:

```cpp
std::unique_ptr<llvm::Module> lowerMLIRToLLVMIR(mlir::ModuleOp cpuModule,
                                                mlir::ModuleOp kernelModule,
                                                llvm::LLVMContext &llvmCtx,
                                                bool targetGPU);
```

Fork Step B of the kernel module processing:

```cpp
if (kernelModule) {
  // Step A: Bufferize (SAME for both CPU and GPU paths)
  {
    // ... existing bufferization code, unchanged ...
  }

  if (targetGPU) {
    // Step B (GPU): linalg → parallel loops → gpu.launch → NVVM → cubin
    mlir::PassManager kernelPM(context);

    // B1: Linalg → parallel loops (scf.parallel instead of scf.for)
    kernelPM.addNestedPass<mlir::func::FuncOp>(
        mlir::createConvertLinalgToParallelLoopsPass());

    // B2: Map scf.parallel dimensions to GPU grid (blocks/threads)
    kernelPM.addPass(mlir::createGpuMapParallelLoopsPass());

    // B3: scf.parallel → gpu.launch
    kernelPM.addPass(mlir::createConvertParallelLoopToGpuPass());

    // B4: Outline gpu.launch body → gpu.func inside gpu.module
    kernelPM.addPass(mlir::createGpuKernelOutliningPass());

    // B5: Lower GPU ops to NVVM inside gpu.module
    kernelPM.addNestedPass<mlir::gpu::GPUModuleOp>(
        mlir::createConvertGpuOpsToNVVMOps());

    // B6: Serialize gpu.module to binary (cubin/fatbin via ptxas)
    // No standalone createGpuSerializeToCubinPass — use the pipeline approach:
    //   1. Attach NVVM target to gpu.module (sets triple, chip, features)
    //   2. gpu-module-to-binary compiles to cubin/fatbin
    // See mlir/Dialect/GPU/Pipelines/Passes.h for GPUToNVVMPipelineOptions.
    // Alternatively, use buildLowerToNVVMPassPipeline() which bundles all of this.
    kernelPM.addPass(mlir::createGpuModuleToBinaryPass());

    // B7: Host-side: gpu.launch_func → cudaLaunchKernel,
    //     gpu.alloc → cudaMalloc, gpu.memcpy → cudaMemcpy, etc.
    kernelPM.addPass(mlir::createGpuToLLVMConversionPass());

    // B8: Cleanup
    kernelPM.addPass(mlir::memref::createExpandStridedMetadataPass());
    kernelPM.addPass(mlir::createLowerAffinePass());

    if (mlir::failed(kernelPM.run(kernelModule)))
      return nullptr;

  } else {
    // Step B (CPU): existing path — linalg → scf.for loops
    // ... existing code, unchanged ...
  }

  // Step C: Merge kernel functions into CPU module (SAME for both paths)
  // ... existing merge code, unchanged ...
}
```

**Verification:** With `--gpu`, a kernel test should produce MLIR with `gpu.launch_func` ops. Without `--gpu`, all existing tests pass unchanged.

---

### Step 4: Register NVVM Dialect Translation for LLVM IR Export

**File: `src/codegen/MLIRLowering.cpp`**

After the merge, before `translateModuleToLLVMIR`, register the GPU dialect translation:

```cpp
#include "mlir/Target/LLVMIR/Dialect/NVVM/NVVMToLLVMIRTranslation.h"

// Before translateModuleToLLVMIR:
mlir::registerBuiltinDialectTranslation(*context);
mlir::registerLLVMDialectTranslation(*context);
if (targetGPU)
  mlir::registerNVVMDialectTranslation(*context);  // NEW
```

Without this, `translateModuleToLLVMIR` will fail on any NVVM ops remaining in the module.

---

### Step 5: Linking with CUDA Runtime

**File: `src/compiler/Compiler.cpp`**

In the link stage, when `--gpu` is active, add CUDA runtime libraries:

```cpp
// In the linker invocation (where clang++/g++ is called):
if (options.gpu) {
  linker_args.push_back("-lcudart");
  linker_args.push_back("-L/usr/local/cuda/lib64");  // or detect via cmake
}
```

The serialized cubin blob is embedded as a global constant in the LLVM module by `gpu-to-cubin`. At runtime, `cudaLaunchKernel` (inserted by `gpu-to-llvm`) loads it from the embedded blob. No separate .ptx file needed.

**Verification:** Linked executable runs on a CUDA-capable machine.

---

### Step 6: End-to-End Test

**File: `e2e-tests/compilables/kernel/kernel_map_gpu.mn`**

```sammine
kernel double_arr(a: [3]f64) -> [3]f64 {
  map(a, (x: f64) -> f64 { x * 2.0 })
}

let main() -> i64 {
  let arr = [1.0, 2.0, 3.0];
  let result = double_arr(arr);
  // result should be [2.0, 4.0, 6.0]
  // verify by checking result[0] == 2.0
  if result[0] == 2.0 { 0 } else { 1 }
}
```

Run with: `./build/bin/sammine -f test.mn --gpu`

---

## Potential Issues & Mitigations

### 1. gpu-module-to-binary requires CUDA toolkit at compile time
`createGpuModuleToBinaryPass` shells out to `ptxas` (from CUDA toolkit) to compile
PTX→cubin. If `ptxas` isn't on `$PATH`, this pass will fail. Mitigation: check for
`ptxas` early and emit a clear error message before running the pass pipeline.

### 2. Memref layout mismatch at GPU boundary
The B2 wrapper builds memrefs with identity layout (`memref<3xf64>`). If GPU passes expect strided layout, there will be a type mismatch. Mitigation: the bufferization already uses `IdentityLayoutMap` option — this should propagate correctly. If not, insert `memref.cast` ops.

### 3. scf.parallel may not tile correctly for large arrays
`createGpuMapParallelLoopsPass` maps loop dimensions 1:1 to GPU grid dimensions. For large arrays, this creates too many threads. Mitigation: add a tiling pass before the parallel loop conversion:

```cpp
// Optional tiling (Step B0, before B1):
// pm.addPass(mlir::createLinalgTilingPass({256}));  // tile to 256-element blocks
```

This is an optimization concern, not a correctness concern — start without tiling.

### 4. Kernel module merge after GPU lowering
After GPU lowering, the kernel module contains `gpu.module` ops (not just `func.func`). The merge step (Step C) currently only moves `func.func` ops. It needs to also move `gpu.module` ops:

```cpp
// In Step C, also move gpu.module ops:
for (auto &op : llvm::make_early_inc_range(kernelModule.getBody()->getOperations())) {
  if (auto funcOp = llvm::dyn_cast<mlir::func::FuncOp>(op)) {
    // ... existing merge logic ...
  } else if (auto gpuMod = llvm::dyn_cast<mlir::gpu::GPUModuleOp>(op)) {
    gpuMod->remove();
    cpuModule.push_back(gpuMod);
  }
}
```

---

## Pass Pipeline Summary

### CPU path (existing, unchanged)
```
kernelModule: bufferize → linalg-to-loops → buffer-opts → expand-strided → lower-affine
merge into cpuModule
cpuModule: scf→cf → arith→llvm → cf→llvm → memref→llvm → func→llvm → reconcile
```

### GPU path (new)
```
codegen: emitKernelWrapper emits gpu.alloc/gpu.memcpy/gpu.dealloc in wrapper
kernelModule: bufferize → linalg-to-parallel-loops → gpu-map-parallel-loops
            → parallel-to-gpu → gpu-kernel-outlining → nvvm-attach-target
            → gpu-to-nvvm → gpu-module-to-binary → gpu-to-llvm
            → expand-strided → lower-affine
merge into cpuModule (func.func + gpu.module ops)
cpuModule: scf→cf → arith→llvm → cf→llvm → memref→llvm → func→llvm → reconcile
link: -lcudart -L/usr/local/cuda/lib64
```

## Required Headers Reference

```cpp
// GPU dialect + pipelines
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "mlir/Dialect/GPU/Pipelines/Passes.h"  // GPUToNVVMPipelineOptions, buildLowerToNVVMPassPipeline

// GPU conversions
#include "mlir/Conversion/SCFToGPU/SCFToGPUPass.h"      // createConvertParallelLoopToGpuPass
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"    // createConvertGpuOpsToNVVMOps (no "Pass" suffix)
#include "mlir/Conversion/GPUCommon/GPUCommonPass.h"     // createGpuToLLVMConversionPass

// NVVM translation
#include "mlir/Target/LLVMIR/Dialect/NVVM/NVVMToLLVMIRTranslation.h"
```

## References

- [MLIR GPU Dialect](https://mlir.llvm.org/docs/Dialects/GPU/)
- [GPU Compilation with MLIR (Stephen Diehl)](https://www.stephendiehl.com/posts/mlir_gpu/)
- [Compiling CUDA with clang (LLVM)](https://llvm.org/docs/CompileCudaWithLLVM.html)
- `plans/gpu_notes.md` — Descend/Lift/Rise research
- `plans/cpu_gpu_boundary_papers.md` — boundary pattern survey
- `plans/mlir_linalg_gpu_api_reference.md` §9 — GPU pass C++ API reference
- `plans/kernel-linalg-codegen.md` — B2 wrapper pattern details
