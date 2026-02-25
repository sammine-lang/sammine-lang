# Codegen Patterns

## TypeConverter (`src/codegen/TypeConverter.cpp`)
- `get_type(Type)`: maps `TypeKind` → `llvm::Type*`
  - `I32_t` → `getInt32Ty`, `I64_t` → `getInt64Ty`, `F64_t` → `getDoubleTy`
- `NumberExprAST` codegen: `I32_t` uses `std::stoi`, `I64_t` uses `std::stoll`, `F64_t` uses `std::stod` — suffix is already stripped by type checker
  - `Pointer` → `llvm::PointerType::get(context, 0)` (opaque pointer)
  - `Bool` → `getInt1Ty`, `Unit` → `getVoidTy`
- `get_cmp_func(Type, Type, TokenType)`: returns `llvm::CmpInst::Predicate`
  - Integer types use `ICMP_*`, float uses `FCMP_*`
  - Pointer uses `ICMP_EQ`/`ICMP_NE` (address comparison, `==`/`!=` only)
  - Unit/Function/etc. → abort (cannot compare)

## CgVisitor (`include/codegen/CodegenVisitor.h`, `src/codegen/CodegenVisitor.cpp`)
- Extends `ScopedASTVisitor`
- `allocaValues` stack: maps variable names to `llvm::AllocaInst*`
- For pointer operations:
  - **Deref (`*p`)**: override `visit()` to traverse operand normally, then `postorder_walk` does `CreateLoad(pointee_type, operand_val)`
  - **AddrOf (`&x`)**: override `visit()` to NOT load the operand — instead get the alloca directly from `allocaValues` and use it as the pointer value
  - **Alloc (`alloc(expr)`)**: `postorder_walk` computes `sizeof(T)` via `DataLayout`, calls `malloc(size)`, stores operand value into returned pointer
  - **Free (`free(expr)`)**: `postorder_walk` calls `free(operand_val)`, sets `ast->val = nullptr`

## Assignment Codegen (`postorder_walk(BinaryExprAST*)`)
The `TokASSIGN` branch supports three LHS forms:
- **`VariableExprAST`** — look up alloca by name, `CreateStore(RHS, alloca)`
- **`DerefExprAST`** — use `operand->val` (the pointer), `CreateStore(RHS, pointer)`
- **`IndexExprAST` with `DerefExprAST` array** — `(*ptr)[i] = val`: GEP through pointer into array element, `CreateStore(RHS, gep)`
- **`IndexExprAST` with `VariableExprAST` array** — `arr[i] = val`: GEP into alloca, `CreateStore(RHS, gep)`

Any future LHS pattern (e.g. struct field access) would add another `dynamic_cast` branch here.

## Pointer-to-Array Indexing (`(*ptr)[i]`)
Both read and assignment through dereferenced pointers to arrays are supported:
- **`visit(IndexExprAST*)`**: if `array_expr` is a `DerefExprAST`, visits only `deref->operand` (to get the pointer value) — does NOT load the whole array
- **`postorder_walk(IndexExprAST*)`**: if `array_expr` is `DerefExprAST`, uses `GEP(arr_type, ptr_val, {0, idx})` + `CreateLoad` for element access
- Bounds checking via `emitBoundsCheck()` applies in both direct and pointer-to-array cases
- Immutable pointer params get `readonly nocapture` LLVM attributes for optimization (`FunctionCodegen.cpp`)

## Array Equality Codegen (`emitArrayComparison`)
- Element-wise comparison via loop: stores arrays to allocas, GEPs into each element, compares
- Short-circuits on first mismatch (branches to `loop_mismatch` → result is `false`)
- If all elements match, loop exits normally → result is `true`
- `!=` is implemented as `CreateNot` of the `==` result
- Nested arrays (e.g. `[[i32;2];3]`) handled via recursive `emitArrayComparison` calls
- Dispatched from `postorder_walk(BinaryExprAST*)` when `lhs_type.type_kind == TypeKind::Array`

## First-Class Functions & Closures

### Closure Representation
- All function values use a fat pointer: `%sammine.closure = type { ptr, ptr }` (code_ptr, env_ptr)
- Named struct created once in `CgVisitor::preorder_walk(ProgramAST*)`
- `TypeConverter::get_type(TypeKind::Function)` returns `%sammine.closure`
- `TypeConverter::get_closure_function_type(FunctionType)` returns `llvm::FunctionType` with env ptr prepended: `ret(ptr, params...)`

### Wrapper Generation (`getOrCreateClosureWrapper`)
- When a named function is used as a value (e.g. `let f = square`), a wrapper is generated:
  ```llvm
  define i32 @__wrap_square(ptr %env, i32 %x) {
    %r = call i32 @square(i32 %x)
    ret i32 %r
  }
  ```
- Wrappers cached in `CgVisitor::closure_wrappers` map to avoid duplicates
- Wrapper accepts-and-ignores the env pointer, forwards remaining args to original function

### Three Call Paths (`postorder_walk(CallExprAST*)` in `FunctionCodegen.cpp`)
1. **Direct call**: `Module->getFunction(name)` found + not partial → `CreateCall(callee, args)` (zero overhead, existing path)
2. **Partial application**: `Module->getFunction(name)` found + `is_partial` → generate `@__partial_N` wrapper with env struct for bound args, return `%sammine.closure`
3. **Indirect call**: callee not in Module (it's a local variable) → load `%sammine.closure` from alloca, `ExtractValue` code/env, `CreateCall(funcType, codePtr, {envPtr, args...})`

### VariableExprAST Function References (`preorder_walk(VariableExprAST*)`)
- If not in `allocaValues` but `Module->getFunction(name)` found + type is Function:
  - Create wrapper via `getOrCreateClosureWrapper()`
  - Build `%sammine.closure { wrapper, null }` via `InsertValue`

### Partial Application Codegen
- Bound args stored in stack-allocated env struct: `alloca { bound_types... }`
- `@__partial_N` wrapper loads bound args from env via GEP+Load, then calls original function
- Unique names via `CgVisitor::partial_counter`
- **Stack limitation**: partial closures cannot escape their defining scope (env is on the stack)

## Forward Declarations
- `preorder_walk(ProgramAST*)` now emits **forward declarations** for ALL user-defined functions before codegen
- Iterates `DefinitionVec`, finds every `FuncDefAST`, and calls `Module->getOrInsertFunction()` with the correct type
- This eliminates definition-ordering constraints — functions can call each other regardless of source order
- Typeclass instance methods (with mangled names like `Sized$i32$sizeof`) are included in this forward declaration pass

## Typeclass Codegen
- Instance methods are codegen'd as regular functions with mangled names (e.g. `Sized$i32$sizeof`)
- `TypeClassDeclAST` and `TypeClassInstanceAST` have no-op visitor stubs — all real work happens via the mangled `FuncDefAST`s
- Calls to typeclass methods use `resolved_generic_name` (the mangled name) for direct function lookup — no vtable, no indirection
- When `is_typeclass_call` is set, codegen uses the mangled name from `resolved_generic_name` instead of `functionName`

## Runtime Function Declarations
- `malloc`, `free`, `printf`, and `exit` are declared in `preorder_walk(ProgramAST*)` alongside the `%sammine.closure` struct type
- `CodegenUtils::declare_malloc()` and `CodegenUtils::declare_free()` in `src/codegen/CodegenUtils.cpp`
- `declare_fn()` is the shared helper for declaring external C functions

## Generics / Monomorphization in Codegen
- `CgVisitor::visit(FuncDefAST*)`: if `ast->Prototype->is_generic()`, return immediately (skip generic templates)
- Monomorphized copies have `type_params` empty and concrete types → normal codegen path
- `TypeKind::TypeParam` → abort in `get_type()` and `get_cmp_func()` (should never reach codegen)
- Monomorphized functions are injected at the **front** of `DefinitionVec` by `Compiler::typecheck()` so they're codegen'd before call sites
- Monomorphizer (`src/typecheck/Monomorphizer.cpp`) deep-clones AST with type substitution; must handle all ExprAST subtypes

## 5 Visitors That Need Updating for Each New AST Node
1. `AstPrinterVisitor` (`src/ast/AstPrinterVisitor.cpp`) — visit + pre/post stubs
2. `BiTypeCheckerVisitor` (`src/typecheck/BiTypeChecker.cpp`) — synthesize + pre/post
3. `CgVisitor` (`src/codegen/CodegenVisitor.cpp`) — codegen in pre/post/visit
4. `ScopeGeneratorVisitor` (`src/semantics/ScopeGeneratorVisitor.cpp`) — empty pre/post stubs
5. `GeneralSemanticsVisitor` (`include/semantics/GeneralSemanticsVisitor.h`) — empty pre/post stubs (inline)

## MLIR Backend (`src/codegen/MLIRGen.cpp`, `src/codegen/MLIRLowering.cpp`)

### Architecture
- **Not** using the visitor pattern — direct recursive dispatch (`emitExpr` → `dynamic_cast` to subtypes)
- Public API: `mlirGen(MLIRContext&, ProgramAST*, moduleName)` → `OwningOpRef<ModuleOp>`
- Lowering: `lowerMLIRToLLVMIR(ModuleOp, LLVMContext&)` → `unique_ptr<llvm::Module>`
- Both backends coexist; selected via `--backend=mlir` (default is `llvm`)

### Dialect mapping
| sammine construct | MLIR dialect |
|---|---|
| Arithmetic (+,-,*,/,%,cmp) | `arith` |
| Function defs, calls, return | `func` |
| if/else (Phase 2) | `scf` |
| Variables, arrays (Phase 2-3) | `memref` |
| Closures, structs, strings, pointers | `llvm` |

### Type conversion (`MLIRGenImpl::convertType`)
- `I32_t`/`I64_t` → `builder.getI32Type()`/`getI64Type()`
- `F64_t` → `builder.getF64Type()`, `Bool` → `getI1Type()`
- `String`/`Pointer` → `LLVM::LLVMPointerType::get(ctx)`
- `Function` → `builder.getFunctionType(params, results)` (extracts param/return types from `FunctionType`)
- `Unit` → `NoneType` (functions returning unit have 0 results)

### Key patterns
- `proto->type` is the full `FunctionType` — must extract return type via `std::get<FunctionType>(proto->type.type_data).get_return_type()`
- Uses existing `LexicalStack<mlir::Value, std::monostate>` for scoped variable tracking
- Generic functions skipped (same as CgVisitor — only monomorphized copies)
- Extern functions emitted as `func.func` declarations with `Private` visibility

### Lowering pipeline (Phase 2)
```
scf-to-cf → arith-to-llvm → memref-to-llvm → cf-to-llvm → func-to-llvm → reconcile-unrealized-casts → translateModuleToLLVMIR
```
Pass order matters: SCF must lower to CF before CF→LLVM; MemRef must lower before the final reconcile pass.

### CMake setup
- MLIR found via `-DMLIR_DIR=...` (not hardcoded)
- `MLIRBackend` OBJECT library links: `MLIRIR`, `MLIRParser`, `MLIRSupport`, `MLIRPass`, `MLIRArithDialect`, `MLIRFuncDialect`, `MLIRSCFDialect`, `MLIRMemRefDialect`, `MLIRArithToLLVM`, `MLIRFuncToLLVM`, `MLIRSCFToControlFlow`, `MLIRControlFlowToLLVM`, `MLIRMemRefToLLVM`, `MLIRReconcileUnrealizedCasts`, `MLIRTargetLLVMIRExport`, `MLIRLLVMToLLVMIRTranslation`, `MLIRBuiltinToLLVMIRTranslation`, `MLIRLLVMDialect`
- Compiler.cpp loads 5 dialects: `arith::ArithDialect`, `func::FuncDialect`, `LLVM::LLVMDialect`, `scf::SCFDialect`, `memref::MemRefDialect`

### Phase 1 implemented expressions
FuncDefAST, ExternAST, NumberExprAST, BoolExprAST, BinaryExprAST (all arith + comparisons), VariableExprAST (immutable SSA), CallExprAST (direct only), ReturnExprAST, BlockAST, VarDefAST (SSA-only), UnaryNegExprAST

### Phase 2 implemented expressions
- **VarDefAST (mutable)** — `let mut x = ...` → `memref.alloca` + `memref.store`; immutable `let` stays SSA
- **VariableExprAST (mutable)** — checks if stored value is `MemRefType` → `memref.load`, else direct SSA
- **IfExprAST** — `scf.if %cond -> (type) { scf.yield %val } else { scf.yield %val }`; Unit-typed if has no result types
- **Assignment** — `x = expr` in `emitBinaryExpr` → `memref.store` to LHS variable's memref

### Phase 2 design notes
- Mutable vs immutable distinguished by the MLIR type of the stored value in `LexicalStack` — `MemRefType` means mutable, else SSA
- `scf.if` chosen over `cf.cond_br` for structured semantics + future optimization (Phase 7 tensor work)
- `func.return` inside `scf.if` is invalid MLIR — early returns in if-branches not yet supported

### File split
- `MLIRGenImpl.h` — class declaration, inline helpers, named constants
- `MLIRGen.cpp` — `generate()`, `convertType()`, `emitDefinition()`, `emitVarDef()`, `emitBlock()`, `getTypeSize()`, `getOrCreateGlobalString()`, `emitAllocaOne()`
- `MLIRGenFunction.cpp` — `emitFunction()`, `emitExtern()`, closure wrappers, partial application, `emitCallExpr()`, `emitFuncCallAndLLVMReturn()`
- `MLIRGenExpr.cpp` — all expression emission (number, bool, string, binary, unary, if, array, pointer, struct, field access)
- `MLIRLowering.cpp` — `lowerMLIRToLLVMIR()` lowering pipeline

### Named constants (`MLIRGenImpl.h`)
```cpp
kClosureTypeName  = "sammine.closure"    kStructTypePrefix = "sammine.struct."
kWrapperPrefix    = "__wrap_"            kPartialPrefix    = "__partial_"
kStringPrefix     = ".str."              kMallocFunc       = "malloc"
kFreeFunc         = "free"               kExitFunc         = "exit"
```

### Inline helpers (`MLIRGenImpl.h`)
- `llvmPtrTy()` — returns `LLVM::LLVMPointerType::get(ctx)` (replaces 14+ repeated calls)
- `llvmVoidTy()` — returns `LLVM::LLVMVoidType::get(ctx)` (replaces 4+ repeated calls)

### Extracted helpers
- `emitAllocaOne(elemType, loc)` — 1-element alloca: `ConstantIntOp(1, i64)` + `LLVM::AllocaOp`
- `emitPtrArrayGEP(ptr, idx, arrType, loc)` — GEP through pointer into array element (used by load/store)
- `emitFuncCallAndLLVMReturn(callee, retType, args, loc)` — call + void-vs-value LLVM return pattern (used in wrappers/partial)

### RAII patterns
- Use `mlir::OpBuilder::InsertionGuard` instead of manual `saveInsertionPoint()`/`restoreInsertionPoint()`
- In `emitPartialApplication`, scope the guard in a `{}` block so it restores before the closure-building code that follows

### `getTypeSize()` alignment
- Struct sizes use `llvm::alignTo()` for ABI-correct padding (e.g. `{i32, f64}` = 16 bytes, not 12)
- Function type = 16 bytes (two pointers for closure struct)
- Array type = element_size * count

### Not yet implemented
Module system/imports

## Build & Test
```bash
# Default backend (LLVM)
cmake --build build -j --target unit-tests e2e-tests

# MLIR backend (requires -DMLIR_DIR when configuring)
cmake -B build -DSAMMINE_TEST=ON \
  -DLLVM_DIR=/path/to/llvm-project/build/lib/cmake/llvm \
  -DMLIR_DIR=/path/to/llvm-project/build/lib/cmake/mlir
./build/bin/sammine -f test.mn --backend=mlir
```
