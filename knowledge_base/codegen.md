# Codegen — MLIR Emission & Lowering

## Architecture
- Direct recursive dispatch (`emitExpr` → `dynamic_cast`), NOT visitor pattern
- `mlirGen()` → `MLIRGenResult{cpuModule, kernelModule}` (kernel null if no kernel defs)
- `lowerMLIRToLLVMIR()` → `unique_ptr<llvm::Module>`
- Files: `MLIRGen.cpp` (main + kernel), `MLIRGenFunction.cpp` (functions/closures/calls), `MLIRGenExpr.cpp` (expressions), `MLIRGenBinaryOps.cpp/.h` (int/float arithmetic free functions), `MLIRLowering.cpp` (passes)

## MIR (in progress, see `plans/mir.md`)
- MIR is the default (`--mir`; `--no-mir` turns it off): before any body is emitted, `lowerThroughMIR` offers each function body (`functionBodies`, the same list `emitDefinition` emits) to `MIRLowering::lowerFunction`. It builds the function apart from the module and, only if the whole body lowers, puts it in place of the AST codegen's declaration, marked `sammine.mir`; otherwise the declaration stays and `emitFunction` gives it a body as before
- Handled so far: functions over scalars (integers, floats, `bool`, `char`) and pointers to them, returning one or `()`; their literals, parameters, `let`, built-in arithmetic, `alloc`, `free`, reading and writing `*p`, statements, `return`, and direct calls to sammine and C functions (`sammine.call`, not yet partial application, function values, enum constructors or built-in instances). A C function is called by its C name (`std::printf` is `printf`, from `ASTProperties::symbol`); an argument passed through its `...` goes as it is, but an owned pointer is lent, as C only reads it. An owned pointer passed to a `ptr<T>` is lent: `lowerArgument` makes it `sammine.lend` of `sammine.deref` of the pointer. A body that runs off its end returns `()`, or its last value: GeneralSemanticsVisitor adds no implicit return to a function that writes `-> unit` (`returnsUnit()` means none written). Control flow is blocks: `lowerStatements` lowers a block's statements and stops once one returns (`terminated()`); `if` is `cf.cond_br` to a block per side and a merge block taking its value (none if both sides return); `&&`/`||` short-circuit through a merge block taking an i1. Comparisons and negation use `emitScalarComparison`/`emitNegation` in `MLIRGenBinaryOps`, shared with the AST codegen. Operators through a built-in instance go through `lowerOperator`, by the operands' sammine type: integers, chars, bools and integer-backed enums (`integerKind`) take integer ops, bitwise ones too; floats float ops; pointers compare their addresses (`sammine.address`, signed); arrays and enums with payloads take `==`/`!=` (`lowerEquality`: a loop over `sammine.element`, or tags then a branch on the tag to compare that variant's payload, never reading another variant's). Every switch MIR makes goes through `branchOn`, which makes one with no cases a plain branch (MLIR's translation crashes on it: `upstream/mlir-switch-no-cases-phi`). `x as T` and an operator through an instance: a built-in instance (no function exists) is `emitNumberConversion` or the operator itself (`lowerBuiltinInstance`), as an explicit call like `Add<i32>::Add(a, b)` is; one the program declares is a `sammine.call` to its method (`callMethod`). Reading `*p` is a `sammine.copy` of a value that owns nothing and a `sammine.move` of one that does (`smn::ownsMemory`). Float constants come from `emitFloatConstant` in `MLIRGenBinaryOps`, shared with the AST codegen
- A MIR function has MIR types in its signature (`!sammine.ptr<i32, owned>` for `'ptr<i32>`). The AST codegen calls every sammine function through `emitSammineCall`, which bridges each argument and result whose type differs from the callee's with `unrealized_conversion_cast`
- `sammine-mir-to-llvm` (`src/dialect/MIRToLLVM.cpp`), first in the lowering to LLVM, turns `!sammine.ptr` into `!llvm.ptr` in signatures, every block's arguments (`FuncLowering`; MLIR's own function pattern converts only the entry block, see `upstream/mlir-func-signature-conversion-blocks`), calls, returns and branches; reconciling then removes the casts. Ops that take or give no MIR type are left alone
- `--emit mlir` starts the dump with `// MIR: N of M functions`. `e2e-tests` runs the suite with MIR, `e2e-tests-ast` without (lit `--param ast=1`, which passes `--no-mir`; a test passing `--mir` itself still gets MIR, as the last of the two wins)
- `SourceLocator` maps AST byte offsets to MLIR locations for both paths
- `sammine.call @f(...)` names its callee and carries MIR's types. When a caller is lowered its callee may still become either kind, so its verifier (`verifySymbolUses`) checks only counts against a callee without `sammine.mir` (`smn::kMirAttr`), and exact types against one with it. `sammine-mir-to-llvm` makes it a `func.call` with LLVM types, which match the callee's either way, or, to a C function declared as an `llvm.func` (variadic, or with structs), an `llvm.call` through `cabi::emitCall`, which promotes variadic arguments as C does. The verifier lets a variadic callee take more arguments than it declares
- Structs and tuples are values: `!sammine.struct<"name", (fields)>` (lowered to the identified LLVM struct `sammine.struct.<name>`, the AST codegen's very type, so values cross between the paths unchanged) and `!sammine.tuple<...>` (a literal LLVM struct). `sammine.aggregate` builds one from its fields' values, in field order (a struct literal's values are lowered in the order written), `sammine.extract` reads a field; `let (a, b) = t` extracts each. A struct that reaches itself through a pointer has no MIR type yet; nor is a struct or tuple passed to or from C lowered (emitCCall does C's struct passing). Once locals are places, field access on one becomes `sammine.field` on a place
- Bindings are values (`names_`), except one whose address the code takes: `AddressedBindings`, a visitor over the body, finds the variable at the root of `&x`, `&x.f`, `&a[i]` or of `x[i]` through an Indexer; `bind` gives such a binding a slot, `sammine.local` (a stack slot when lowered), and stores its value. Bindings are immutable and nothing writes through a view, so the slot keeps the value. `hasPlace`/`lowerPlace` (mirroring the AST codegen's `has_address`) name places: a slotted binding, `*p`, `p[i]`, and fields (`sammine.field`) or checked elements (`sammine.elem`) of a place; a field or element of a place is read from it, so only it is copied or moved. `&x` is `sammine.addr` of the place: a view that, unlike `sammine.lend`, may outlive a call (`let p = &x`). `x[i]` through an Indexer lends its method x's place, or a slot holding a temporary
- Function values are `!sammine.fn<(A) -> R>`, lowered to the AST codegen's closure `sammine.closure` `{code, env}`. `sammine.closure @f(bound...)` is f with its first arguments bound: a function named as a value binds none (`__wrap_<f>`, null env, the same wrapper the AST codegen makes, reused if it exists); a partial application keeps what it binds in a stack slot of the function making it, read by a `__partial_N` wrapper. A view a partial application binds is `sammine.addr`, not `lend`: it lasts as the closure does (`lowerArgument`'s `ViewLasts`). `sammine.call_indirect` calls through a value, env first. `callableFromMIR` says what MIR can't call yet (an array to C, a sammine function's array result). A kernel's wrapper takes arrays by pointer on the CPU and as memrefs on the GPU (`kernel_abi::memrefOfPointer`, in `SammineCABI`, shared with the kernel codegen) and fills an array it returns in a buffer passed last, which its declaration marks `smn::kResultBufferAttr`: `sammine.call`'s lowering (`callKernel`) passes arrays through stack slots as each parameter's type says. The verifiers of `sammine.call` and `sammine.closure` read a C function's signature as a sammine call sees it (`sammineSignature`: an sret buffer is a result)
- Arrays are values: `!sammine.array<N, T>` (lowered to `!llvm.array<N x T>`, the AST codegen's type), a literal a `sammine.aggregate` (all number constants: a constant global `.mir.const_arr.N` instead, as the AST codegen does), `len` a constant, `dim` a tuple of constant sizes; a range array `[a...b]` is a slot (`sammine.local`) filled by a loop through `sammine.elem`, then read. `a[i]` is checked by MIR code of its own (`checkBounds`: compare as i64, then a block calling `dprintf` and `exit` that ends in `sammine.unreachable`) before `sammine.element` reads it. Not yet: functions returning an array (sret), arrays to C or a kernel wrapper, writing `a[i]`, array `==`
- Enums with payloads are values too: `!sammine.enum<"name", N, (payload tuples)>`, N the payload's bytes, lowered to the AST codegen's identified struct `sammine.enum.<name>` (`{i32 tag, [N x i8]}`). `sammine.variant` builds one, `sammine.tag` reads its tag, `sammine.payload` a payload field; both carry byte offsets from `layout::` (`TypeLayout`), the AST codegen's own rules, so values are the same bytes on either path. `case` is `cf.switch` on the tag to a block per arm (with no wildcard, other tags go to the last arm), joined like an `if` (`lowerArms`). `for i in a..b` (`lowerFor`) is the AST codegen's loop in MIR: `Range { a, b }` is a header block's argument, each turn calls `Iterator<Range<T>, T>::next` and branches on its tag; the Some block binds i and goes round with the next range. The Option type and `next`'s name come from `rangeIteration`, shared with `emitForExpr`. An integer-backed enum is its backing integer (a variant its discriminant, `==` an integer compare); a `case` on one, or on literals (integers, chars, bools), is `cf.switch` on the value (`lowerScalarCase`). A char is a signed integer, as in the AST codegen. Not yet: `==` of enums with payloads. Types print their element lists through `custom<TypeList>`, which allows an empty list (`!sammine.tuple<>`)
- MIR ops so far: `sammine.string` (a string literal, a `!sammine.ptr<i8, view>` nothing owns; lowered to the address of a constant global `.mir.str.N`, one per distinct string in the module), `sammine.alloc`, `sammine.free`, `sammine.deref` (a pointer's `!sammine.place<T>`), `sammine.ptr_elem` (the place `p[i]`, in an unsafe function; a GEP over the pointee type), `sammine.lend` (a `!sammine.ptr<T, view>` of a place, which its verifier lets only `sammine.call` use, as a view lasts for the call), `sammine.copy`, `sammine.move`, `sammine.store`; `sammine-mir-to-llvm` turns them into `malloc` (size from the data layout, count widened to i64), `free`, the pointer itself (for both `deref` and `lend`), loads and stores, as the AST codegen emits them. Their builders take the result type explicitly
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

## C Functions
- `emitExtern` declares a C binding under its C name (`std::printf` is `printf`). The symbol for any name is NameResolver's `ASTProperties::symbol`: the AST codegen (`mangleName`) and MIR name callees through it. A variadic one, or one taking or giving a struct, is an `llvm.func`, called with `llvm.call` by `emitCCall`; any other is a `func.func`
- The C calling convention lives in `SammineCABI` (`include/cabi/`, `src/cabi/`), apart from codegen so MIR's lowering to LLVM can use it too: `registerType`/`registersFor` say how a struct travels (registers, or memory as sret/byval), `callC` calls C with sammine's values, a struct passed as C passes it (register type or byval copy) and a struct result through its register type or an sret buffer, given a way to make stack slots: the AST codegen's `emitCCall` and MIR's lowering of `sammine.call` (and of a C function's closure wrapper) both use it; `emitCall` makes an `llvm.call` to a C function with arguments in its declared types: it applies C's default promotions to an argument passed through `...` (f32 to f64, char sign-extended and bool zero-extended to i32), and copies the declaration's argument attributes (sret, byval, signext, zeroext) onto the call; `createString` makes the constant global holding a string as C reads it (its bytes and a NUL), for both the AST codegen's `getOrCreateGlobalString` and MIR's string literals

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
- A kernel's scalar code, a map lambda's body and a reduce's identity, goes through MIR: `MIRLowering::lowerKernelScalar` lowers it where the kernel codegen's builder is, the lambda's parameter bound, and it must be plain `arith` ops in the one block a linalg region has (`lowerArithOnly`), else a located error. MLIRGenImpl always has a MIRLowering for this

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
