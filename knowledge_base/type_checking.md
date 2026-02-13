# Type Checking Patterns

## Type System (`include/typecheck/Types.h`)
- `TypeKind` enum: `I32_t`, `I64_t`, `F64_t`, `Unit`, `Bool`, `String`, `Function`, `Pointer`, `Record`, `Never`, `NonExistent`, `Poisoned`
- `TypeData` variant: `std::variant<FunctionType, PointerType, std::string, std::monostate>`
- Factory methods: `Type::I32_t()`, `Type::Pointer(pointee)`, `Type::Function(params)`, etc.
- `PointerType` stores pointee as `std::shared_ptr<Type>` (not by value) because `Type` is forward-declared when `PointerType` is defined. `FunctionType` avoids this with `std::vector<Type>` (heap-allocated internally).

## Equality
- `Type::operator==` compares `type_kind` first, then `type_data` only for `Function` and `Pointer` kinds (other kinds are fully identified by their `TypeKind`)

## BiTypeChecker (`include/typecheck/BiTypeChecker.h`)
- Bidirectional: `synthesize()` produces types bottom-up, `postorder_walk()` checks consistency top-down
- Two lexical stacks: `id_to_type` (variable/function names) and `typename_to_type` (type names like "i32", "f64")
- Built-in types registered in `enter_new_scope()`: i32, i64, f64, bool, unit
- `resolve_type_expr(TypeExprAST*)` resolves structured type AST nodes:
  - `nullptr` → `Type::NonExistent()`
  - `SimpleTypeExprAST` → lookup in `typename_to_type`
  - `PointerTypeExprAST` → recursive resolve, wrap in `Type::Pointer()`
- Default integer (no decimal point) synthesizes as `I32_t`; with decimal → `F64_t`
- `main` function must return `i32` (checked in `postorder_walk(PrototypeAST*)`)

## Alloc/Free Type Rules
- `alloc(expr)`: operand type T synthesized first, result type = `ptr<T>`
- `free(expr)`: operand must be `ptr<T>`, errors if not pointer, result type = `unit`

## Adding a New Type Checklist
1. Add to `TypeKind` enum in `Types.h`
2. Add class to `TypeData` variant if compound (like `PointerType`, `FunctionType`)
3. Add factory method on `Type` struct
4. Add `to_string()` case
5. Register in `enter_new_scope()` if it's a named type
6. Handle in all `switch(type_kind)` statements (compiler warns via `-Wswitch`)
7. Add `case` to `TypeConverter::get_type()` and `get_cmp_func()`

## Adding a New AST Node to Type Checker
1. Add `synthesize()` declaration in `BiTypeChecker.h`
2. Add `preorder_walk()` and `postorder_walk()` declarations in `BiTypeChecker.h`
3. Implement all three in `BiTypeChecker.cpp`
