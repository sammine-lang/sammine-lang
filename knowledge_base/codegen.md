# Codegen — MLIR Emission & Lowering

## Architecture
- Direct recursive dispatch (`emitExpr` → `dynamic_cast`), NOT visitor pattern
- `mlirGen()` → `MLIRGenResult{cpuModule, kernelModule}` (kernel null if no kernel defs)
- `lowerMLIRToLLVMIR()` → `unique_ptr<llvm::Module>`
- Files: `MLIRGen.cpp` (main + kernel), `MLIRGenFunction.cpp` (functions/closures/calls), `MLIRGenExpr.cpp` (expressions), `MLIRGenBinaryOps.cpp/.h` (int/float arithmetic free functions), `MLIRLowering.cpp` (passes)

## Type Mapping
| TypeKind | MLIR Type |
|---|---|
| I32/U32/Integer | i32 |
| I64/U64 | i64 |
| F64/Flt | f64 |
| F32 | f32 |
| Bool | i1 |
| Char | i8 |
| Unit | NoneType (0 results) |
| String/Pointer | `!llvm.ptr` |
| Function | `!llvm.struct<"sammine.closure", (ptr, ptr)>` |
| Array | `LLVMArrayType` |
| Struct | named struct in `structTypes` map |
| Enum | `{ i32 tag, [N x i8] payload }` or integer backing type |
| Tuple | `LLVMStructType::getLiteral(elementTypes)` |

## Variable Model
- All non-array vars: alloca + store (uniform for mut/immut)
- Arrays: alloca returns `!llvm.ptr`; `emitVariableExpr` returns pointer directly (no load)
- Named constants: `.str.` (strings), `.const_arr.` (immutable all-literal arrays → globals)

## Handler Dispatch Pattern
Two chains use `optional`-return: non-null = handled, nullptr = "not my job", loop over `static constexpr Handler handlers[]`.

- **`emitVarDef`**: TupleDestructure → Array → Scalar
- **`emitBinaryExpr`**: IntArith → FloatArith → Comparison → EnumBitwise → TypeclassOp
- Assignment (`=`) short-circuits before the chain

## Forward Declarations (`generate()`)
1. Register types: all StructDef/EnumDef → MLIR types
2. Forward-declare: all FuncDef/ExternAST/TypeClassInstance methods
3. (If kernels) 3 declarations per kernel: `__kernel_` in kernel module (tensor), `__kernel_` in CPU module (memref placeholder), wrapper in CPU module (public)

## Closures & Partial Application
- Fat pointer: `{ code_ptr, env_ptr }`
- Three call paths: direct (zero overhead), partial (env struct for bound args), indirect (load + extract + call)
- Wrapper `__wrap_<name>` generated when named function used as value
- **Stack limitation**: closures cannot escape defining scope

## Enum Codegen
- Layout: `{ i32 tag, [N x i8] payload }` (unit-only: just `{ i32 }`)
- Construction: alloca + GEP + store tag and payload
- Pattern matching: cascading `cmpi` + `cf::CondBranchOp` + merge blocks

## Case Expression Abstractions
- `emitScalarCaseExpr`: shared for integer-backed enum and literal patterns; parameterized by `ArmToComparisonConst` lambda
- `emitPayloadCaseExpr`: tag extraction + cascading dispatch + payload binding via byte-offset GEP
- `emitArrayComparison`: loop-based element-by-element `==`/`!=` (header/body/exit blocks)

## Global Const Arrays
- Immutable arrays with all-literal elements → `llvm.mlir.global` with `.const_arr.` prefix
- Initializer: PoisonOp + InsertValueOp per element

## Kernel Codegen (2-Module Architecture)

### Invariants
- Kernel module: only arith/math/tensor/linalg/func ops — NO llvm dialect
- `in_kernel_lambda_body` flag MUST be true inside linalg body builders — prevents alloca/malloc/free
- DPS: array-returning kernels get output as last param, never allocate internally
- Kernel wrappers: `sammine.kernel_wrapper` attr → `emitCallExpr` passes arrays as `!llvm.ptr`

### Kernel Type Conversion (`convertTypeForKernel`)
- `Array` → `RankedTensorType` (default) or `MemRefType` (post-bufferization)
- Rejected: Pointer, Function, String, Struct, Enum, Tuple, nested arrays

### Emission
- `emitKernelMapExpr`: `linalg::MapOp` with body builder; DPS output = last block arg
- `emitKernelReduceExpr`: 0-d output via `tensor::EmptyOp` + `FillOp` + `linalg::ReduceOp`; extract scalar via `tensor::ExtractOp`
- `emitKernelWrapper`: `buildMemrefFromPtr` per array input + sret; calls `__kernel_` with memref args

### `buildMemrefFromPtr`
Constructs `{ allocated_ptr, aligned_ptr, offset=0, sizes={N}, strides={1} }` from raw `!llvm.ptr`, cast to `memref<NxT>` via `unrealized_conversion_cast`.

### Bufferization Pipeline (kernel module only)
```
one-shot-bufferize (IdentityLayoutMap) → linalg-to-loops → buffer-hoisting → buffer-loop-hoisting → promote-buffers-to-stack (64KB) → expand-strided-metadata → lower-affine
```

### Module Merge
1. Erase CPU module's memref placeholder for each `__kernel_`
2. Move bufferized definition into CPU module
3. Standard MLIR→LLVM lowering on unified module

### MLIR→LLVM Lowering
```
scf-to-cf → arith-to-llvm → cf-to-llvm → finalize-memref-to-llvm → func-to-llvm → reconcile-unrealized-casts
```

### Post-Lowering Attributes
- All functions: `nounwind`
- malloc return: `noalias`
- Kernel wrappers: `noinline` (preserves LICM)

## GPU Codegen (Phase 7c — `--gpu=cuda`)

### Overview
When `--gpu=cuda` is passed, kernel functions compile to CUDA GPU code via MLIR's
GPU dialect pipeline. The kernel runs on the GPU; the host wrapper handles data
marshalling. Commits: `6f6c38a`..`14c77b3`.

### Architecture Changes (vs CPU path)

**Wrapper signature (GPU):** array args become `memref<NxT>` (not `!llvm.ptr`).
The wrapper body is pure func/memref/gpu dialect — no LLVM ops. This is required
because `gpu-to-llvm` needs to convert everything in one partial conversion pass.

**Call site:** when calling a kernel wrapper with `--gpu`, `emitCallExpr` converts
`!llvm.ptr` → memref via `buildMemrefFromPtr` at the call site (not inside the wrapper).
The `unrealized_conversion_cast` at the call site gets folded by `reconcile-unrealized-casts`.

**Wrapper body (GPU path):** uses async gpu ops with `gpu.wait` barriers:
```
%t0 = gpu.wait async
%dev, %t1 = gpu.alloc async [%t0] : memref<NxT>
%t2 = gpu.memcpy async [%t1] %dev, %host_memref
gpu.wait [%t2]            // barrier: host→device copy done
call @__kernel_*(%dev, %dev_out)
%t3 = gpu.wait async
%t4 = gpu.memcpy async [%t3] %host_out, %dev_out
%t5 = gpu.dealloc async [%t4] %dev_out
gpu.wait [%t5]            // barrier: device→host copy done
```
**Why async:** `gpu-to-llvm` only converts async gpu ops (checked by
`isAsyncWithOneDependency` in `ConvertAllocOpToGpuRuntimeCallPattern`).
Synchronous ops are left unconverted. The `gpu.wait` barriers make execution
synchronous despite using async ops.

### GPU Lowering Pipeline (kernel module, `MLIRLowering.cpp`)
```
bufferize (shared with CPU)
→ linalg-to-parallel-loops (scf.parallel, not scf.for)
→ gpu-map-parallel-loops (nested under func::FuncOp)
→ convert-parallel-loop-to-gpu (scf.parallel → gpu.launch)
→ gpu-kernel-outlining (gpu.launch → gpu.func in gpu.module)
→ gpu-nvvm-attach-target (tag gpu.module with nvptx64 triple + sm_75)
→ gpu-to-nvvm (inside gpu.module: gpu.thread_id → nvvm intrinsics)
→ lower-affine, arith-to-llvm, index-to-llvm, reconcile (inside gpu.module)
→ gpu-module-to-binary (ptxas compiles to cubin, embedded as constant)
→ expand-strided-metadata, lower-affine
```

### Phase 2 (merged CPU module)
```
scf-to-cf
→ gpu-to-llvm (replaces func-to-llvm + memref-to-llvm; converts gpu.alloc→mgpuMemAlloc, etc.)
→ reconcile-unrealized-casts
```
**Key:** `gpu-to-llvm` replaces the separate `func-to-llvm` + `memref-to-llvm` passes
for the GPU path. It handles all dialect-to-LLVM conversion in one partial conversion.

### Module Merge (Step C)
Moves `func::FuncOp`, `gpu::GPUModuleOp`, and `gpu::BinaryOp` from kernel module
into CPU module. Sets `gpu.container_module` attribute on CPU module (required by
`gpu.launch_func`).

### ConvertToLLVM Interface Registration
`gpu-to-llvm` collects patterns from all loaded dialects via `ConvertToLLVMPatternInterface`.
All extensions must be registered BEFORE any dialects are loaded:
```cpp
// In Compiler.cpp, before any getOrLoadDialect calls:
mlir::registerAllExtensions(earlyRegistry);
mlir::registerConvertToLLVMDependentDialectLoading(earlyRegistry);
gpu::registerOffloadingLLVMTranslationInterfaceExternalModels(earlyRegistry);
```

### cuModuleUnload Teardown Fix
MLIR registers `cuModuleUnload` as a global destructor (priority 123), but the CUDA
driver deinitializes first → error at exit. Fix: strip `llvm.global_dtors` and insert
explicit unload calls before each `ret` in `main()` (`MLIRLowering.cpp`).

### Linking
Executables link against `libmlir_cuda_runtime.so` (provides `mgpuMemAlloc`,
`mgpuMemcpy`, `mgpuStreamCreate`, etc. which wrap CUDA driver API). Library path
derived from `MLIR_DIR` at cmake configure time (`MLIR_LLVM_LIB_DIR` define).

### Known Issues
- `linalg.reduce` → GPU segfaults at runtime (XFAIL test: `kernel_reduce_gpu.mn`)
- `--gpu=amd` flag accepted but not implemented
- No tiling — 1 GPU thread per array element (fine for small arrays)
- Kernel composition (`kernel calling kernel`) not supported at syntax level

### Lit Test Infrastructure
`lit.cfg.py` checks for `ptxas` on PATH → adds `cuda` feature. Tests use
`REQUIRES: cuda` to gate on CUDA availability.

## Dialect Usage
| Construct | Dialect |
|---|---|
| Arithmetic, comparisons | arith |
| Function defs/calls/return | func |
| if/else, while, case/match | cf (ControlFlow) |
| Bounds checks | scf (scf.if + exit) |
| Closures, structs, enums, strings, pointers | llvm |
| Kernel tensor ops, map, reduce | linalg, tensor |
| GPU memory, kernel launch | gpu (alloc, memcpy, dealloc, launch_func) |
| GPU device code | nvvm (thread_id, block_id intrinsics) |
