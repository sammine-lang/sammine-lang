# Codegen — MLIR Emission & Lowering

## Architecture
- `mlirGen()` → `MLIRGenResult{cpuModule, kernelModule}` (kernel null if no kernel defs); `lowerMLIRToLLVMIR()` → `unique_ptr<llvm::Module>`
- `MLIRGenImpl` builds the module around the code: a declaration of every function in MIR types (`forwardDeclareFunc`, `mirFunctionType`), C bindings (`emitExtern`), kernels (`MLIRGenKernel.cpp`). Every function body goes through MIR: `lowerThroughMIR` gives each declaration its body through `MIRLowering::lowerFunction`, before any definition is emitted; one MIR can't lower is an error (`imm_error`). `emitFunction` only sets linkage
- Files: `MLIRGen.cpp` (generate, types, definitions), `MLIRGenFunction.cpp` (declarations, C bindings, linkage), `MLIRGenKernel.cpp` (kernels), `MIRLowering.cpp` (bodies and kernels' scalar code → MIR: statements, the dispatch over expressions, literals; `MIRLowering{Calls,ControlFlow,Aggregates,Places,Operators}.cpp` the rest, by concern), `MIRTypes.cpp` (each sammine type's MIR type), `MLIRGenBinaryOps.cpp` (arith helpers: arithmetic, comparison, negation, conversion), `TypeLayout.cpp` (enum payload layout), `RangeIteration.cpp` (what `for` calls), `MLIRLowering.cpp` (passes); `src/dialect/` (MIR, `sammine-mir-to-llvm`); `src/cabi/` (C's and kernels' calling conventions)
- `SourceLocator` maps AST byte offsets to MLIR locations

## MIR (see `plans/mir.md`)
`MIRLowering` lowers a type-checked, monomorphized body to MIR in the `sammine` dialect; `sammine-mir-to-llvm` (`src/dialect/MIRToLLVM.cpp`), first in the lowering to LLVM, turns it into LLVM dialect. A function MIR defines is marked `sammine.mir` (`smn::kMirAttr`). Every function is declared in MIR types, which the pass converts; a C binding that is an `llvm.func` (variadic, or taking or giving a struct) and a kernel's wrapper are in LLVM types already, a C binding's from its MIR types through `llvmTypeOf`.

### Values and places
- Bindings are the values they name (`names_`), except one whose address the code takes: `AddressedBindings`, a visitor over the body, finds the variable at the root of `&x`, `&x.f`, `&a[i]` or of `x[i]` through an Indexer; `bind` gives it a slot, `sammine.local`, holding its value. Bindings are immutable and nothing writes through a view, so the slot keeps it
- Places (`!sammine.place<T>`): a slotted binding, `*p` (`sammine.deref`), `p[i]` in an unsafe function (`sammine.ptr_elem`), and fields (`sammine.field`) or checked elements (`sammine.elem`) of a place; `hasPlace`/`lowerPlace` name them. A place is read with `sammine.copy` (owns nothing) or `sammine.move` (owns memory, `smn::ownsMemory`), written with `sammine.store`. A field or element of a place is read from it, so only it is copied or moved
- `&x` is `sammine.addr`, a view of a place for as long as it lives (`let p = &x`). An owned pointer passed to a `ptr<T>` is lent for the call: `sammine.lend` of its pointee, which only `sammine.call` may use; one a partial application binds lasts as the closure does, so it is `sammine.addr` (`lowerArgument`'s `ViewLasts`)

### Types (MIR → LLVM)
| sammine | MIR | LLVM |
|---|---|---|
| integers, floats, bool, char | `i32`/`i64`/`f32`/`f64`/`i1`/`i8` | same |
| `'ptr<T>` / `ptr<T>` | `!sammine.ptr<T, owned/view>` | `!llvm.ptr` |
| struct | `!sammine.struct<"name", (fields)>` | identified `sammine.struct.<name>` |
| tuple | `!sammine.tuple<...>` | literal struct |
| `[N]T` | `!sammine.array<N, T>` | `!llvm.array<N x T>` |
| enum with payloads | `!sammine.enum<"name", N, (payload tuples)>` | identified `sammine.enum.<name>`, `{i32 tag, [N x i8]}` |
| integer-backed enum | its backing integer | same |
| function value | `!sammine.fn<(A) -> R>` | identified `sammine.closure`, `{code, env}` |

There is one mapping from sammine types to LLVM's: `MIRTypes`, then the pass's `MIRTypeConverter` (`llvmTypeOf` for what needs LLVM types before the pass). A struct or enum that reaches itself through a pointer has no MIR type. Element lists print through `custom<TypeList>`, which allows an empty one (`!sammine.tuple<>`).

### Values
- Literals: numbers are `arith.constant`; a string is `sammine.string`, a `!sammine.ptr<i8, view>` nothing owns, lowered to a constant global `.mir.str.N` (one per distinct string, `cabi::createString`)
- Structs, tuples, arrays: `sammine.aggregate` builds one (a struct literal's values lowered in the order written), `sammine.extract` reads a field of a value; `let (a, b) = t` extracts each. An array of number constants is a constant global `.mir.const_arr.N`. `a[i]` of a value is checked first by MIR code (`checkBounds`: compare as i64, then a block calling `dprintf` and `exit` that ends in `sammine.unreachable`), then `sammine.element` reads it. `len` is a constant, `dim` a tuple of constant sizes; `[a...b]` fills a slot in a loop through `sammine.elem`
- Enums: `sammine.variant` builds one, `sammine.tag` reads its tag, `sammine.payload` a payload field; both carry byte offsets from `layout::` (`TypeLayout`), so an enum is the same bytes in every function. An integer-backed enum's variant is its discriminant
- Function values: `sammine.closure @f(bound...)` is f with its first arguments bound. A function named as a value binds none: `__wrap_<f>`, null env. A partial application keeps what it binds in a stack slot of the function making it, read by a `__partial_N` wrapper

### Control flow
- `lowerStatements` lowers a block's statements, stopping once one returns (`terminated()`). A body that runs off its end returns `()`, or its last value: GeneralSemanticsVisitor adds no implicit return to a function that writes `-> unit` (`returnsUnit()` means none written)
- `if`: `cf.cond_br` to a block per side; `case` on an enum with payloads: `cf.switch` on its tag, each arm binding its payload; a `case` on an integer-backed enum or on literals: `cf.switch` on the value (`lowerScalarCase`). With no wildcard, other values go to the last arm. Arms join in a block taking the value (`lowerArms`); none if every arm returns. `&&`/`||` short-circuit through a block taking an i1. Every switch goes through `branchOn`, which makes one with no cases a plain branch (MLIR's translation crashes on it: `upstream/mlir-switch-no-cases-phi`)
- `for i in a..b` (`lowerFor`): `Range { a, b }` is a header block's argument; each turn calls `Iterator<Range<T>, T>::next` and branches on its tag; Some binds i and goes round with the next range. The Option type and `next`'s name come from `rangeIteration`

### Calls
- `sammine.call @f(...)` to a sammine or C function by its symbol (`ASTProperties::symbol`: a C binding's is its C name). Its verifier (`verifySymbolUses`) checks exact types against a MIR callee and counts against a declaration, read as a sammine call sees it (`sammineSignature`: a C sret buffer, or a kernel wrapper's result buffer, is a result). An argument through a C `...` goes as it is, but an owned pointer is lent
- Lowered: to a MIR callee, a `func.call`; to a C function declared as an `llvm.func`, `cabi::callC` (structs as C passes them, variadic promotions); to a kernel's wrapper, `callKernel` (arrays through stack slots, by pointer or memref as each parameter's type says, a returned array read from the result buffer)
- `sammine.call_indirect` calls through a function value, env first. A method of an instance the program declares (an operator, `as` through Convert, `x[i]` through an Indexer, which lends x's place or a slot holding a temporary) is a `sammine.call` (`callMethod`)

### Operators
- Through a built-in instance, by the operands' sammine type (`lowerOperator`): integers, chars (signed), bools and integer-backed enums (`integerKind`) take integer ops, bitwise ones too; floats float ops; pointers compare their addresses (`sammine.address`, signed); arrays and enums with payloads take `==`/`!=` (`lowerEquality`: a loop over `sammine.element`, or tags, then a branch on the tag to compare that variant's payload). `x as T` through a built-in Convert is `emitNumberConversion`; an explicit call like `Add<i32>::Add(a, b)` goes through `lowerOperator` too. Comparison, negation and float constants come from `MLIRGenBinaryOps`

### Memory
- `sammine.alloc`/`sammine.free` lower to `malloc` (size from the data layout, count widened to i64) and `free`; a place is its pointer, a view lent or taken of it the same pointer; `sammine.local`, and what calls to C or kernels pass through memory, are stack slots at the top of the function (`entrySlot`)

### Lowering to LLVM, the dialect, tools
- `sammine-mir-to-llvm` converts a function's signature and entry block with MLIR's function pattern, and every other block through the branches into it (`cf::populateCFStructuralTypeConversionsAndLegality`; see `upstream/mlir-func-signature-conversion-blocks`)
- The dialect lives in `include/dialect/`, `src/dialect/`: `SammineDialect.td`, `SammineTypes.td`, `SammineOps.td`. Libraries: `SammineDialect` (types and ops; linking it brings the generated headers, on the `SYSTEM` include path) and `SammineDialectTransforms` (`MIRToLLVM`, and `LowerSammineOps` for the GPU transfer ops)
- `sammine-opt` (`tools/sammine-opt`) is `MlirOptMain` with the sammine, func, arith and cf dialects; lit runs `.mlir` tests with `%sammine_opt` (`e2e-tests/mir/dialect/`). `--emit mlir` prints MIR

## Forward Declarations (`generate()`)
1. Forward-declare: all FuncDef/ExternAST/TypeClassInstance methods, in MIR types
2. (If kernels) 3 declarations per kernel: `__kernel_` in kernel module (tensor), `__kernel_` in CPU module (memref placeholder), wrapper in CPU module (public)
3. `lowerThroughMIR` replaces each function's declaration with its MIR definition

## C Functions
- `emitExtern` declares a C binding under its C name (`std::printf` is `printf`). A variadic one, or one taking or giving a struct, is an `llvm.func`; any other is a `func.func`
- The calling conventions live in `SammineCABI` (`include/cabi/`, `src/cabi/`): `registerType`/`registersFor` say how a struct travels (registers, or memory as sret/byval); `callC` calls C with sammine's values given a way to make stack slots; `emitCall` makes the `llvm.call`, promoting arguments through `...` (f32 to f64, char sign-extended and bool zero-extended to i32) and copying the declaration's argument attributes (sret, byval, signext, zeroext) onto the call; `createString` makes a C string's constant global; `kernel_abi::memrefOfPointer` makes a kernel's memref of an array's address

## Symbols
- `emitFunction(ast, Linkage)`. `Global`: in a library every function and instance method, private ones too (its generic code compiled in another module may call them; privacy is a naming rule); in an executable only `main`. `Internal`: an executable's functions, and generic instantiations (`FuncDefAST::is_instantiation`) everywhere, so two modules can both instantiate `vec::push<i32>`. `AvailableExternally`: an imported instance's methods, copies to inline; MLIRLowering adds `inlinehint` (without it `Iterator<Range>::next` costs 245 against a threshold of 225)
- Forward declarations start private: an imported instance method is never given a body, and MLIR rejects a public declaration
- A function an IR test inspects must be in a library (no `main`), or the optimizer may inline or remove it

## Kernel Codegen (2-Module Architecture)
- A kernel's scalar code, a map lambda's body and a reduce's identity, goes through MIR: `MIRLowering::lowerKernelScalar` lowers it where the kernel codegen's builder is, the lambda's parameter bound, and it must be plain `arith` ops in the one block a linalg region has (`lowerArithOnly`), else a located error. MLIRGenImpl always has a MIRLowering for this

### Invariants
- Kernel module: only arith/math/tensor/linalg/func ops — NO llvm dialect
- A linalg body holds only what `lowerKernelScalar` accepts: plain arith ops in one block
- DPS: array-returning kernels get output as last param, never allocate internally
- Kernel wrappers: `sammine.kernel_wrapper` attr; an array-returning one fills a buffer passed last, its argument marked `smn::kResultBufferAttr`. A call to one is `sammine.call`, lowered by `callKernel` (see MIR, Calls)

### Kernel Type Conversion (`convertTypeForKernel`)
- `Array` → `RankedTensorType` (default) or `MemRefType` (post-bufferization)
- Rejected: Pointer, Function, String, Struct, Enum, Tuple, nested arrays

### Emission
- `emitKernelMapExpr`: `linalg::MapOp` with body builder; DPS output = last block arg
- `emitKernelReduceExpr`: 0-d output via `tensor::EmptyOp` + `FillOp` + `linalg::ReduceOp`; extract scalar via `tensor::ExtractOp`
- `emitKernelWrapper`: `kernel_abi::memrefOfPointer` per array input + the result buffer; calls `__kernel_` with memref args

### `kernel_abi::memrefOfPointer` (`include/cabi/KernelABI.h`)
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
sammine-mir-to-llvm → scf-to-cf → arith-to-llvm → cf-to-llvm → finalize-memref-to-llvm → func-to-llvm → reconcile-unrealized-casts
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
| Function bodies | sammine (MIR), lowered to llvm |
| Arithmetic, comparisons | arith |
| Function defs/calls/return | func |
| if/else, for, case, `&&`/`\|\|`, bounds checks | cf (ControlFlow) |
| Kernel tensor ops, map, reduce | linalg, tensor |
| GPU memory, kernel launch | gpu (alloc, memcpy, dealloc, launch_func) |
| GPU device code | nvvm (thread_id, block_id intrinsics) |
