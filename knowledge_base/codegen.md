# Codegen — MLIR Emission & Lowering

## Architecture
- Direct recursive dispatch (`emitExpr` → `dynamic_cast`), NOT visitor pattern
- `mlirGen()` → `MLIRGenResult{cpuModule, kernelModule}` (kernel null if no kernel defs)
- `lowerMLIRToLLVMIR()` → `unique_ptr<llvm::Module>`
- Files: `MLIRGen.cpp` (main + kernel), `MLIRGenFunction.cpp` (functions/closures/calls), `MLIRGenExpr.cpp` (expressions), `MLIRGenBinaryOps.cpp/.h` (int/float arithmetic free functions), `MLIRLowering.cpp` (passes)

## MIR (in progress, see `plans/mir.md`)
- `--mir`: before any body is emitted, `lowerThroughMIR` offers each function body (`functionBodies`, the same list `emitDefinition` emits) to `MIRLowering::lowerFunction`. It builds the function apart from the module and, only if the whole body lowers, puts it in place of the AST codegen's declaration, marked `sammine.mir`; otherwise the declaration stays and `emitFunction` gives it a body as before
- Handled so far: functions over scalars (integers, floats, `bool`, `char`) and pointers to them, returning one or `()`; their literals, parameters, `let`, built-in arithmetic, `alloc`, `free`, reading and writing `*p`, statements, `return`, and direct calls to other sammine functions (`sammine.call`, not yet partial application, function values, enum constructors, built-in instances, C functions, or lending an owned pointer to a `ptr<T>`). Reading `*p` is a `sammine.copy` of a value that owns nothing and a `sammine.move` of one that does (`smn::ownsMemory`). Float constants come from `emitFloatConstant` in `MLIRGenBinaryOps`, shared with the AST codegen
- A MIR function has MIR types in its signature (`!sammine.ptr<i32, owned>` for `'ptr<i32>`). The AST codegen calls every sammine function through `emitSammineCall`, which bridges each argument and result whose type differs from the callee's with `unrealized_conversion_cast`
- `sammine-mir-to-llvm` (`src/dialect/MIRToLLVM.cpp`), first in the lowering to LLVM, turns `!sammine.ptr` into `!llvm.ptr` in signatures, calls, returns and branches; reconciling then removes the casts. Ops that take or give no MIR type are left alone
- `--mir --emit mlir` starts the dump with `// MIR: N of M functions`. `e2e-tests-mir` runs the whole suite with `--mir` (lit `--param mir=1`, feature `mir`)
- `SourceLocator` maps AST byte offsets to MLIR locations for both paths
- `sammine.call @f(...)` names its callee and carries MIR's types. When a caller is lowered its callee may still become either kind, so its verifier (`verifySymbolUses`) checks only counts against a callee without `sammine.mir` (`smn::kMirAttr`), and exact types against one with it. `sammine-mir-to-llvm` makes it a `func.call` with LLVM types, which match the callee's either way
- MIR ops so far: `sammine.alloc`, `sammine.free`, `sammine.deref` (a pointer's `!sammine.place<T>`), `sammine.copy`, `sammine.move`, `sammine.store`; `sammine-mir-to-llvm` turns them into `malloc` (size from the data layout, count widened to i64), `free`, the pointer itself, loads and stores, as the AST codegen emits them. Their builders take the result type explicitly
- The dialect lives apart from codegen, in `include/dialect/` and `src/dialect/`: `SammineDialect.td` (the dialect), `SammineTypes.td` (MIR types: `!sammine.ptr<T, owned|view>` so far, its ownership an `Ownership` enum, so the assembly format is declarative), `SammineOps.td` (ops). Libraries: `SammineDialect` (types and ops; linking it brings the generated headers) and `SammineDialectTransforms` (`MIRToLLVM`, and `LowerSammineOps` for the GPU transfer ops). Codegen, which builds IR from the AST, and `sammine-opt` link them
- `sammine-opt` (`tools/sammine-opt`) is `MlirOptMain` with the sammine, func, arith and cf dialects; lit runs `.mlir` tests with `%sammine_opt`, e.g. round trips and `-verify-diagnostics` (`e2e-tests/mir/dialect/`)

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
- All non-array vars: alloca + store
- Arrays: alloca returns `!llvm.ptr`; `emitVariableExpr` returns pointer directly (no load)
- Named constants: `.str.` (strings), `.const_arr.` (immutable all-literal arrays → globals)

## Handler Dispatch Pattern
Two chains use `optional`-return: non-null = handled, nullptr = "not my job", loop over `static constexpr Handler handlers[]`.

- **`emitVarDef`**: TupleDestructure → Array → Scalar
- **`emitBinaryExpr`**: TypeclassOp → IntArith → FloatArith → Comparison → EnumBitwise (a user's instance first: its type may have built-in operators too)
- Assignment (`=`) and `&&`/`||` are handled before the chain: `emitShortCircuit` branches on the LHS and evaluates the RHS in its own block, so `i < n && xs[i] > 0` never reads `xs[n]`. The linear checker treats that RHS as a branch that may not run (consuming there is "consumed in some branches but not others")

## Forward Declarations (`generate()`)
1. Register types: all StructDef/EnumDef → MLIR types. Structs are declared first and their fields filled in a second loop, so order never matters (`Box<Pt>` is instantiated at the front, before `Pt`)
2. Forward-declare: all FuncDef/ExternAST/TypeClassInstance methods
3. (If kernels) 3 declarations per kernel: `__kernel_` in kernel module (tensor), `__kernel_` in CPU module (memref placeholder), wrapper in CPU module (public)

## Symbols
- `emitFunction(ast, Linkage)`. `Global`: in a library every function and instance method, private ones too (its generic code compiled in another module may call them; privacy is a naming rule); in an executable only `main`. `Internal`: an executable's functions, and generic instantiations (`FuncDefAST::is_instantiation`) everywhere, so two modules can both instantiate `vec::push<i32>`. `AvailableExternally`: an imported instance's methods, copies to inline; MLIRLowering adds `inlinehint` (without it `Iterator<Range>::next` costs 245 against a threshold of 225)
- Forward declarations start private: an imported instance method is never given a body, and MLIR rejects a public declaration
- A function an IR test inspects must be in a library (no `main`), or the optimizer may inline or remove it

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

## GPU Codegen (Phase 7c)

See [gpu_codegen.md](gpu_codegen.md) for the full GPU compilation story.

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
