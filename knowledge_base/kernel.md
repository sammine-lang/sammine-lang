# Kernel Codegen

## Overview
Kernels are pure functions for parallel computation. `map` and `reduce` are the core primitives. Kernel code shares the same expression syntax as CPU code (unified front-end: parser, AST, type checker), but emits to tensor/linalg MLIR dialects instead of LLVM dialect. No `device<T>` vs `host<T>` types -- the compiler decides placement. Address space is determined by a context flag during codegen, not at type-checking time.

## AST

| Node | Role | Parent |
|---|---|---|
| `KernelDefAST` | Top-level `kernel name(params) -> T { body }` | `DefinitionAST` |
| `KernelExprAST` | Base class for kernel-only expressions | `AstBase` (parallel to `ExprAST`) |
| `KernelMapExprAST` | `map(array, (x: T) -> U { body })` | `KernelExprAST` |
| `KernelReduceExprAST` | `reduce(array, op, identity)` | `KernelExprAST` |
| `KernelNumberExprAST` | Integer/float literal in kernel body | `KernelExprAST` |
| `KernelBlockAST` | Holds `vector<unique_ptr<KernelExprAST>>` | Plain class (not AstBase) |

- `KernelExprAST` does **not** participate in the CPU visitor pattern -- `accept_vis`/`walk_with_preorder`/`walk_with_postorder` are no-op stubs, `accept_synthesis` returns `NonExistent`.
- `KernelMapExprAST` reuses CPU AST nodes (`PrototypeAST` + `BlockAST`) for the lambda, enabling unified type checking of the lambda body.
- `KernelReduceExprAST` stores the operator as a `Token` (`op_tok`) and the identity as a CPU `ExprAST`.
- Files: `include/ast/Ast.h` (lines 929-1038), `include/ast/AstBase.h` (NodeKind enum)

## Type Checking

### `visit(KernelDefAST*)` (`BiTypeChecker.cpp`)
Opens a new scope, sets `ctx().enclosing_kernel`, visits the prototype, then calls `synthesize(KernelDefAST*)`. Does not recurse into the kernel body via CPU visitors.

### `synthesize(KernelDefAST*)` (`BiTypeCheckerSynthesize.cpp`)
Iterates `Body->expressions`, dispatching to `synthesize_kernel_map` or `synthesize_kernel_reduce` via `dyn_cast`. `KernelNumberExprAST` assumes the declared return type. Verifies body result type matches the prototype's declared return type (with polymorphic numeric resolution).

### `synthesize_kernel_map`
1. Looks up `input_name` in scope, verifies it is an `Array` type
2. Checks lambda has exactly 1 parameter
3. Opens a new scope, synthesizes the lambda parameter
4. Verifies param type matches array element type
5. Resolves lambda's declared return type
6. Visits lambda body, checks body type matches declared return type
7. Returns `Type::Array(declared_ret, arr_size)` -- same size, element type = lambda return type

### `synthesize_kernel_reduce`
1. Looks up `input_name` in scope, verifies `Array` type
2. Validates operator is `+`, `-`, `*`, or `/` (token types `TokADD`, `TokSUB`, `TokMUL`, `TokDIV`)
3. Requires **concrete** numeric element type (rejects polymorphic numerics)
4. Synthesizes identity expression, resolves polymorphic numeric literals to element type
5. Checks identity type is compatible with element type
6. Returns the **element type** (reduce collapses array to scalar)

### Pre-registration
`visit(ProgramAST*)` second pass pre-registers kernel prototypes (same as `FuncDefAST`), enabling forward references to kernels.

## 2-Module Architecture

```
MLIRGenResult { cpuModule, kernelModule }
```

- **cpuModule**: contains all CPU code (LLVM dialect), plus forward declarations for `__kernel_` (memref types) and the public wrapper.
- **kernelModule**: contains kernel functions using tensor/linalg/arith dialects. Created lazily on first `KernelDefAST` (null if no kernels).

### Three forward declarations per kernel (in `generate()`)
1. `__kernel_<name>` in **kernel module** -- tensor types, `Private` visibility
2. `__kernel_<name>` in **CPU module** -- memref types, `Private` visibility (placeholder replaced during merge)
3. `<name>` (public wrapper) in **CPU module** -- CPU ABI types (`!llvm.ptr` for array params, sret for array returns)

The wrapper gets a `sammine.kernel_wrapper` unit attribute.

- File: `src/codegen/MLIRGen.cpp` (generate, lines 107-171)
- Result struct: `include/codegen/MLIRGen.h` (`MLIRGenResult`)
- `--mlir-ir` flag dumps both modules separately for debugging

## Kernel Type System

### `convertTypeForKernel(type, asMemref=false)` (`MLIRGen.cpp`)

| TypeKind | MLIR Type (asMemref=false) | MLIR Type (asMemref=true) |
|---|---|---|
| `Array` | `RankedTensorType<NxElem>` | `MemRefType<NxElem>` |
| `I32_t`, `Integer`, `U32_t` | `i32` | `i32` |
| `I64_t`, `U64_t` | `i64` | `i64` |
| `F64_t`, `Flt` | `f64` | `f64` |
| `F32_t` | `f32` | `f32` |
| `Bool` | `i1` | `i1` |
| `Char` | `i8` | `i8` |
| `Unit` | `NoneType` | `NoneType` |

**Not allowed** in kernel context: `Pointer`, `Function`, `String`, `Struct`, `Enum`, `Tuple` -- triggers `imm_error`. Nested arrays also not supported.

### `buildKernelFuncType(proto, asMemref=false)`
Builds `func::FunctionType` for a kernel. For array-returning kernels, appends the output array as an extra argument (DPS convention). `asMemref=false` for pre-bufferization tensor types; `asMemref=true` for post-bufferization memref types used by the wrapper.

## Codegen

### `emitKernelDef` (`MLIRGen.cpp`)
Two phases:
1. **Internal kernel function** (`__kernel_<name>`) -- emitted into `kernelModule`. Parameters bound as tensor SSA values (no alloca). DPS output is the last entry block argument for array-returning kernels. Dispatches to `emitKernelMapExpr`, `emitKernelReduceExpr`, or handles `KernelNumberExprAST` inline.
2. **Public wrapper** -- calls `emitKernelWrapper` to emit into `cpuModule`.

### `in_kernel_lambda_body` guard
Flag on `MLIRGenImpl` (`include/codegen/MLIRGenImpl.h`). Set to `true` inside `linalg.map`/`linalg.reduce` body builders. Prevents emitting LLVM ops (`alloca`, `malloc`, `free`) that are invalid inside linalg body regions -- only `arith`/`math` ops are valid there. Saved/restored around each body builder call.

### `emitKernelMapExpr`
1. Looks up input tensor from symbol table
2. Uses DPS output tensor (passed as parameter, not `tensor.empty()`) -- kernels never allocate
3. Creates `linalg::MapOp` with body builder lambda
4. Inside body: redirects `this->builder` into linalg body region (`InsertionGuard`), pushes symbol table scope, registers lambda parameter, sets `in_kernel_lambda_body = true`, emits lambda body via `emitExpr`, yields result via `linalg::YieldOp`
5. Returns result tensor via `func::ReturnOp`

### `emitKernelReduceExpr`
1. Looks up input tensor
2. Emits identity value via `emitExpr`
3. Creates 0-d (scalar) output tensor via `tensor::EmptyOp`, fills with identity via `linalg::FillOp`
4. Creates `linalg::ReduceOp` with dimensions `{0}` and body builder
5. Inside body: emits appropriate arith op based on operator token (float: `AddFOp`/`SubFOp`/`MulFOp`/`DivFOp`; int: `AddIOp`/`SubIOp`/`MulIOp`/`DivSIOp` or `DivUIOp` for unsigned)
6. Extracts scalar from 0-d result via `tensor::ExtractOp`
7. Returns scalar via `func::ReturnOp`

### `emitKernelWrapper`
Marshals CPU types to/from kernel types:
- **Input arrays**: `!llvm.ptr` (pass-by-reference) -> `memref` via `buildMemrefFromPtr`
- **Scalar inputs**: passed through directly
- **DPS output**: sret `!llvm.ptr` -> `memref` via `buildMemrefFromPtr`, passed as last argument to `__kernel_`
- Calls `__kernel_<name>` with memref-typed arguments
- Array returns: kernel writes directly into sret memref (zero alloc, zero copy). Scalar returns: forwarded from call result.

### `buildMemrefFromPtr` (`MLIRGen.cpp`)
Constructs an LLVM memref descriptor struct `{ allocated_ptr, aligned_ptr, offset, sizes[1], strides[1] }` from a raw `!llvm.ptr`. Both pointers = input ptr, offset = 0, stride = 1. Cast to `memref<NxT>` via `UnrealizedConversionCastOp`.

## Bufferization Pipeline

Kernel module bufferized separately in `lowerMLIRToLLVMIR()` (`src/codegen/MLIRLowering.cpp`):

```
one-shot-bufferize (IdentityLayoutMap, bufferizeFunctionBoundaries=true)
  -> linalg-to-loops
  -> buffer-hoisting
  -> buffer-loop-hoisting
  -> promote-buffers-to-stack (64KB limit)
  -> expand-strided-metadata
  -> lower-affine
```

Key options:
- `setFunctionBoundaryTypeConversion(IdentityLayoutMap)` -- ensures `memref<NxT>` at function boundaries (not `memref<NxT, strided<[?], offset: ?>>`) to match `buildMemrefFromPtr` output
- No `allowUnknownOps` or `noAnalysisFuncFilter` needed -- kernel module contains only `func`/`tensor`/`linalg`/`arith` ops

## Module Merge

After kernel bufferization + lowering (`MLIRLowering.cpp`, Step C):

1. Iterate all `func::FuncOp` in kernel module
2. Erase the memref-typed forward-declaration in CPU module (the placeholder from step 2 of forward declarations)
3. Move the actual bufferized definition into CPU module via `funcOp->remove()` + `cpuModule.push_back(funcOp)`
4. Standard MLIR->LLVM lowering runs on the unified CPU module: `scf-to-cf -> arith-to-llvm -> cf-to-llvm -> memref-to-llvm -> func-to-llvm -> reconcile-unrealized-casts`

Post-lowering LLVM IR annotations:
- All functions get `nounwind` (sammine has no exceptions)
- `malloc` return gets `noalias`
- Kernel wrappers get `noinline` to preserve LICM (LLVM can hoist the call out of loops when ptr args point to loop-invariant data)

## DPS (Destination-Passing Style)

Array-returning kernels use destination-passing style:

1. `buildKernelFuncType`: appends output array type as last argument
2. `emitKernelDef`: DPS output = last entry block argument
3. `emitKernelMapExpr`: uses DPS output as `init` for `linalg::MapOp` (not `tensor.empty()`)
4. `emitKernelWrapper`: wraps sret `!llvm.ptr` as memref, passes to `__kernel_` as last argument
5. Kernel writes result directly into the sret buffer -- zero allocation, zero copy

Scalar-returning kernels (e.g. `reduce`) do not use DPS -- they return the scalar value directly.

## E2E Tests

| File | Tests |
|---|---|
| `e2e-tests/compilables/kernel/kernel_basic.mn` | Scalar-returning kernel (number literal) |
| `e2e-tests/compilables/kernel/kernel_map.mn` | `map` with f64 lambda: `x * 2.0` |
| `e2e-tests/compilables/kernel/kernel_reduce.mn` | `reduce` with `+` operator |
| `e2e-tests/compilables/kernel/kernel_no_malloc.mn` | Golden test: asserts 0 `malloc` calls in LLVM IR |
