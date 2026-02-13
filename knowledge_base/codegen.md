# Codegen Patterns

## TypeConverter (`src/codegen/TypeConverter.cpp`)
- `get_type(Type)`: maps `TypeKind` → `llvm::Type*`
  - `I32_t` → `getInt32Ty`, `I64_t` → `getInt64Ty`, `F64_t` → `getDoubleTy`
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

## Runtime Function Declarations
- `malloc` and `free` are declared in `preorder_walk(ProgramAST*)` alongside `printf`
- `CodegenUtils::declare_malloc()` and `CodegenUtils::declare_free()` in `src/codegen/CodegenUtils.cpp`
- `declare_fn()` is the shared helper for declaring external C functions

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
