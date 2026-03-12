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

## Dialect Usage
| Construct | Dialect |
|---|---|
| Arithmetic, comparisons | arith |
| Function defs/calls/return | func |
| if/else, while, case/match | cf (ControlFlow) |
| Bounds checks | scf (scf.if + exit) |
| Closures, structs, enums, strings, pointers | llvm |
| Kernel tensor ops, map, reduce | linalg, tensor |
