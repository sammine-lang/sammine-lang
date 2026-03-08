# CPU/GPU Boundary — Paper Reading List

## Most Relevant to sammine-lang (B2 wrapper pattern)

### Futhark — compiler-generated C API wrappers
**"Futhark: purely functional GPU-programming with nested parallelism and in-place array updates"**
Troels Henriksen et al. PLDI 2017.
Closest to sammine-lang's approach: compiles pure functional array programs to GPU kernels, generates C API wrapper functions that handle argument marshalling, kernel dispatch, and memory transfers. Programmer never writes host code. Opaque array types hide memory layout.
[PDF](https://futhark-lang.org/publications/pldi17.pdf)

**"Design and Implementation of the Futhark Programming Language" (PhD Thesis)**
Troels Henriksen. University of Copenhagen, Nov 2017.
Full compilation pipeline including generated host wrappers, size-dependent types, flattening transformation.
[PDF](https://futhark-lang.org/publications/troels-henriksen-phd-thesis.pdf)

### CUDA/NVCC — the original stub pattern
**NVIDIA CUDA Compiler Driver (NVCC) Documentation**
For each `__global__` function, NVCC replaces the body with a stub containing `cudaLaunchKernel` calls. Device code compiled to PTX/SASS, embedded as fat binary. `__cuda_register_globals` registers kernels at startup.
[Docs](https://docs.nvidia.com/cuda/cuda-compiler-driver-nvcc/)

**"Compiling CUDA with clang" (LLVM Docs)**
Merged parsing (unlike NVCC's split compilation). Implementation in `clang/lib/CodeGen/CGCUDANV.cpp` — good reference for how stubs are generated.
[Docs](https://llvm.org/docs/CompileCudaWithLLVM.html) | [Source](https://github.com/llvm/llvm-project/blob/main/clang/lib/CodeGen/CGCUDANV.cpp)

### MLIR GPU — kernel outlining
**MLIR GPU Dialect Documentation**
`gpu.launch` → `gpu.launch_func` via kernel outlining (extracts kernel body into `gpu.func` inside `gpu.module`). This is the MLIR-canonical host-device split.
[GPU Dialect](https://mlir.llvm.org/docs/Dialects/GPU/)

**"One-Shot Function Bufferization of Tensor Programs"** (MLIR Open Meeting, Jan 2022)
Directly addresses the function boundary bufferization problem — tensor args/results become memrefs, layout maps must be chosen.
[Slides](https://mlir.llvm.org/OpenMeetings/2022-01-13-One-Shot-Bufferization.pdf)

### Mojo — unified model, context flag
**"Mojo: MLIR-Based Performance-Portable HPC Science Kernels on GPUs"**
SC Workshops 2025. arXiv:2509.21039.
First language built on MLIR. No explicit host-device boundary — compiler manages data placement based on usage context. Address space annotations applied during codegen via context flag (not at type level). Vendor-agnostic: same code runs on NVIDIA H100 and AMD MI300A.
[arXiv](https://arxiv.org/abs/2509.21039)

## Broader Context

### Halide — algorithm/schedule separation
**"Halide: A Language and Compiler for Optimizing Parallelism, Locality, and Recomputation"**
Ragan-Kelley et al. PLDI 2013.
Schedule annotations (`gpu_blocks()`, `gpu_threads()`) trigger GPU codegen. Compiler auto-inserts host-side buffer allocation, data copies, kernel launch. Supports heterogeneous CPU+GPU with automatic transfers at stage boundaries.
[PDF](https://people.csail.mit.edu/jrk/halide-pldi13.pdf)

### Triton — Python JIT to PTX
**"Triton: An Intermediate Language and Compiler for Tiled Neural Network Computations"**
Tillet, Kung, Cox. MAPL 2019.
`@triton.jit` walks Python AST → Triton-IR → LLVM → PTX. Host Python handles grid config, arg marshalling, launch. Kernels receive raw pointers.
[PDF](https://www.eecs.harvard.edu/~htk/publication/2019-mapl-tillet-kung-cox.pdf)

### SYCL — single-pass vs multi-pass
**"One Pass to Bind Them: The First Single-Pass SYCL Compiler"**
Alpay, Heuveline. IWOCL 2023.
Contrasts SMCP (separate host/device passes) vs SSCP (single unified IR, late outlining). SSCP enables cross-boundary optimization. MLIR's nesting can represent both in one module.
[PDF](https://cdrdv2-public.intel.com/786536/Heidelberg_IWOCL__SYCLCon_2023_paper_2566-1.pdf)

### Polygeist — raising C to MLIR
**"Polygeist: Raising C to Polyhedral MLIR"**
Moses et al. PACT 2021.
Raises C/C++ to MLIR affine dialect, enables auto-parallelization and GPU codegen. Also see their PPoPP 2023 paper on GPU→CPU transpilation using high-level parallel constructs.
[PDF](https://c.wsmoses.com/papers/Polygeist_PACT.pdf)

### Julia GPU — JIT wrapper
**"Effective Extensible Programming: Unleashing Julia on GPUs"**
Besard et al. IEEE TPDS, 2019.
`@cuda` macro triggers JIT compilation to PTX via LLVM. Host-device boundary is thin JIT wrapper. CuArray types mirror Julia arrays for data transfer.
[arXiv](https://arxiv.org/abs/1712.03112)

### IREE — HAL abstraction
**IREE Compiler and Runtime Architecture**
HAL (Hardware Abstraction Layer) designed ~1:1 with compute-only Vulkan. Host VM manages module loading, command buffers, dispatch. Clean separation of host scheduling and device execution.
[Docs](https://iree.dev/)

## Boundary Pattern Summary

| System | Pattern | Data Transfer | sammine-lang parallel |
|--------|---------|---------------|----------------------|
| Futhark | Compiler-generated C API wrappers | Opaque types, explicit API | B2 wrapper pattern |
| CUDA | `__global__` stub replacement | Explicit cudaMemcpy | B2 wrapper pattern |
| MLIR | `gpu.launch` → kernel outlining | memref descriptors | Future GPU path |
| Mojo | Implicit, context flag in codegen | Compiler-managed | Design principle match |
| Halide | Schedule-driven codegen | Auto at stage boundaries | Future scheduling |
| Triton | Python @jit decorator | Via PyTorch tensor ptrs | Different paradigm |
