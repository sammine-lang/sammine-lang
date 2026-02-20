# Codegen Patterns

## TypeConverter (`src/codegen/TypeConverter.cpp`)
- `get_type(Type)`: maps `TypeKind` → `llvm::Type*`
  - `I32_t` → `getInt32Ty`, `I64_t` → `getInt64Ty`, `F64_t` → `getDoubleTy`
- `NumberExprAST` codegen: `I32_t` uses `std::stoi`, `I64_t` uses `std::stoll`, `F64_t` uses `std::stod` — suffix is already stripped by type checker
  - `Pointer` → `llvm::PointerType::get(context, 0)` (opaque pointer)
  - `Bool` → `getInt1Ty`, `Unit` → `getVoidTy`
- `get_cmp_func(Type, Type, TokenType)`: returns `llvm::CmpInst::Predicate`
  - Integer types use `ICMP_*`, float uses `FCMP_*`
  - Pointer/Unit/Function/etc. → abort (cannot compare)

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

## Build & Test
```bash
cmake --build build -j --target unit-tests e2e-tests
```
