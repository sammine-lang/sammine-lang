# Type Checking Patterns

## Type System (`include/typecheck/Types.h`)
- `TypeKind` enum: `I32_t`, `I64_t`, `F64_t`, `Unit`, `Bool`, `String`, `Function`, `Pointer`, `Record`, `Never`, `NonExistent`, `Poisoned`
- `TypeData` variant: `std::variant<FunctionType, PointerType, std::string, std::monostate>`
- Factory methods: `Type::I32_t()`, `Type::Pointer(pointee)`, `Type::Function(params)`, etc.
- `PointerType` stores pointee as `std::shared_ptr<Type>` (not by value) because `Type` is forward-declared when `PointerType` is defined. `FunctionType` avoids this with `std::vector<Type>` (heap-allocated internally).

## Equality & Qualifiers
- `Type::operator==` compares fundamental type structure only (`type_kind` + `type_data`). Qualifiers like `is_mutable` are intentionally ignored — use `compatible_to_from()` for directional checks.
- `Type::is_mutable`: tracks whether the binding is mutable (`let mut` or `mut` param). Default `false`.
- `Type::is_literal()`: returns `true` for primitive types (i32, i64, f64, bool, unit, string, Integer, Flt). Primitive types bypass the mutability check in `compatible_to_from` since they are always by-value.
- `compatible_to_from(to, from)` rejects `immut → mut` for non-primitive types: `to.is_mutable && !from.is_mutable && !from.is_literal()`

## Mutability
- Variables are **immutable by default**: `let x: i32 = 5;`
- Use `let mut` for mutable locals: `let mut x: i32 = 5; x = 10;`
- Use `mut` for mutable function params: `let f(mut x: i32) -> i32 { x = x + 1; ... }`
- Reassigning an immutable variable emits a type-check error with location at the `=` token
- Mutability is set on `Type::is_mutable` during `synthesize(VarDefAST*)` and `synthesize(TypedVarAST*)`
- Assignment check in `synthesize(BinaryExprAST*)`: if LHS is a `VariableExprAST` with `!type.is_mutable`, error

## BiTypeChecker (`include/typecheck/BiTypeChecker.h`)
- Bidirectional: `synthesize()` produces types bottom-up, `postorder_walk()` checks consistency top-down
- Two lexical stacks: `id_to_type` (variable/function names) and `typename_to_type` (type names like "i32", "f64")
- Built-in types registered in `enter_new_scope()`: i32, i64, f64, bool, unit
- `resolve_type_expr(TypeExprAST*)` resolves structured type AST nodes:
  - `nullptr` → `Type::NonExistent()`
  - `SimpleTypeExprAST` → lookup in `typename_to_type`
  - `PointerTypeExprAST` → recursive resolve, wrap in `Type::Pointer()`
- Default integer (no decimal point) synthesizes as `I32_t`; with decimal → `F64_t`
- Number literal type suffixes (`i32`, `i64`, `f64`) override the default: `synthesize(NumberExprAST*)` finds the first alpha char, extracts the suffix, strips it from `ast->number`, and sets the type — invalid suffixes abort with an error
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
