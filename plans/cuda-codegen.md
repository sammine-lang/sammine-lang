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

### Two-phase fork in MLIRLowering.cpp

The kernel module lowering (Step B in `lowerMLIRToLLVMIR`) gets a GPU branch:

```
if (targetGPU) {
  kernelModule:
    bufferize (same as today)
    → linalg → parallel loops (scf.parallel instead of scf.for)
    → map parallel loops to GPU grid dimensions
    → scf.parallel → gpu.launch
    → gpu-kernel-outlining (extracts device code into gpu.module/gpu.func)
    → convert-gpu-to-nvvm (gpu ops → NVVM dialect inside gpu.module)
    → serialize to cubin (compile device code to binary blob)
    → gpu-to-llvm (host-side launch stubs + gpu.alloc/memcpy/dealloc → cudaRuntime calls)
    → merge into cpuModule
  cpuModule: existing pipeline (unchanged)
} else {
  existing CPU path (unchanged)
}
```

### Data marshalling (the hard part)

The B2 wrapper function `@double_arr` currently does:

```
1. receives !llvm.ptr (pointing to caller's array)
2. builds memref descriptor from ptr
3. bufferization.to_tensor → tensor
4. calls @__kernel_double_arr(tensor) → tensor
5. bufferization.materialize_in_destination → writes result to sret ptr
```

For GPU, the wrapper must additionally handle host↔device transfers. Two options:

**Option A: Let MLIR passes handle it (recommended first)**
- After `gpu-kernel-outlining`, the outlined kernel uses memref args
- `gpu-to-llvm` pass automatically inserts `cudaMalloc`/`cudaMemcpy`/`cudaFree` for memref args passed to `gpu.launch_func`
- The wrapper stays mostly the same — the passes transform the call from `func.call @__kernel_*` into the GPU launch sequence
- This is the path of least resistance

**Option B: Explicit marshalling in codegen (if Option A is insufficient)**
- Emit `gpu.alloc`, `gpu.memcpy`, `gpu.launch_func`, `gpu.memcpy`, `gpu.dealloc` directly in the wrapper
- More control, but more codegen work
- Needed if we want async transfers, pinned memory, or multi-stream execution

Start with Option A.

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

### Step 2: GPU Lowering Pipeline

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

### Step 3: Register NVVM Dialect Translation for LLVM IR Export

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

### Step 4: Linking with CUDA Runtime

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

### Step 5: End-to-End Test

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

### 1. gpu-to-cubin requires CUDA toolkit at compile time
The `createGpuSerializeToCubinPass` shells out to `ptxas` or links against `libcuda`. If CUDA isn't installed, this pass will fail. Mitigation: guard behind `MLIR_ENABLE_CUDA_RUNNER` cmake check, provide clear error message.

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

### 5. The wrapper function may need restructuring
Currently the wrapper calls `func.call @__kernel_*` which operates on memrefs after bufferization. After GPU outlining, the kernel body is extracted into a `gpu.func` inside `gpu.module`, and the original function becomes a `gpu.launch_func` call. The `gpu-to-llvm` pass converts this into `cudaLaunchKernel` with marshalled args. This *should* happen automatically if the passes run on the kernel module before merging — the wrapper just sees a regular function that internally does a GPU launch.

If this doesn't work transparently, Option B (explicit `gpu.alloc`/`gpu.memcpy` in wrapper codegen) is the fallback.

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
kernelModule: bufferize → linalg-to-parallel-loops → gpu-map-parallel-loops
            → parallel-to-gpu → gpu-kernel-outlining → gpu-to-nvvm
            → gpu-module-to-binary → gpu-to-llvm → expand-strided → lower-affine
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
