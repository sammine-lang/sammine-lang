# Type Checking Patterns

## Type System (`include/typecheck/Types.h`)
- `TypeKind` enum: `I32_t`, `I64_t`, `F64_t`, `Unit`, `Bool`, `String`, `Function`, `Pointer`, `Array`, `Record`, `Never`, `NonExistent`, `Poisoned`, `Integer`, `Flt`
- `TypeData` variant: `std::variant<FunctionType, PointerType, ArrayType, std::string, std::monostate>`
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
  - `ArrayTypeExprAST` → recursive resolve, wrap in `Type::Array()`
  - `FunctionTypeExprAST` → recursive resolve params + return, wrap in `Type::Function()`
- Default integer (no decimal point) synthesizes as `I32_t`; with decimal → `F64_t`
- Number literal type suffixes (`i32`, `i64`, `f64`) override the default: `synthesize(NumberExprAST*)` finds the first alpha char, extracts the suffix, strips it from `ast->number`, and sets the type — invalid suffixes abort with an error
- `main` function must return `i32` (checked in `postorder_walk(PrototypeAST*)`)

## Scope Lookup Helpers
- `get_type_from_id(str)`: current scope only (aborts if not found)
- `get_type_from_id_parent(str)`: parent scope only (aborts if not found)
- `try_get_callee_type(str)`: recursive search through current + all parent scopes (returns `nullopt` if not found) — used for call expressions and variable references
- `synthesize(VariableExprAST*)` uses `recursive_get_from_name()` to find names in any ancestor scope

## First-Class Functions & Partial Application
- `CallExprAST` has `callee_func_type` (the resolved `Type` of the callee) and `is_partial` flag
- **Partial detection** happens in `synthesize(CallExprAST*)`: if `args.size() < params.size()`, set `is_partial = true`
- **Partial type**: `Type::Function(remaining_params..., return_type)` — e.g. `add(5)` where `add: (i32, i32) -> i32` → type is `(i32) -> i32`
- **Full call type**: just the function's return type (existing behavior)
- `visit(CallExprAST*)` uses `try_get_callee_type()` to search all scopes — fixes lookup for function-typed parameters in current scope

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

## Monomorphized Generics

### Overview
Generic functions use **implicit type parameter discovery** (no `<T>` syntax) and **monomorphization** (concrete copies per type combination). Unknown type names in function signatures are auto-discovered as type parameters.

### Key Types & Fields
- `TypeKind::TypeParam`: represents an unresolved type parameter; uses `std::string` variant of `TypeData`
- `PrototypeAST::type_params`: populated during type checking (not parsing); `is_generic()` checks if non-empty
- `CallExprAST::resolved_generic_name`: mangled name (e.g. `"identity.i32"`)
- `CallExprAST::type_bindings`: e.g. `{"T": i32}`

### Type Checker Flow
1. **Discovery** (`visit(FuncDefAST*)`): sets `in_prototype_context = true`, resolves prototype. In `resolve_type_expr()`, unknown `SimpleTypeExprAST` names are auto-registered as `TypeParam` and added to `discovered_type_params`. If any found → store in `generic_func_defs`, skip body.
2. **Generic call** (`synthesize(CallExprAST*)`): checks `generic_func_defs` first. If found: synthesize arg types, `unify()` each against generic param types to build bindings, compute mangled name, `substitute()` return type.
3. **Monomorphization** (`visit(CallExprAST*)`): if `resolved_generic_name` set and not yet instantiated: `Monomorphizer::instantiate()` deep-clones AST with type substitution, runs `GeneralSemanticsVisitor` (implicit return wrapping), then type-checks the clone as a normal function. Stores in `monomorphized_defs`.
4. **Injection** (`Compiler::typecheck()`): moves `monomorphized_defs` to front of `DefinitionVec` so they're codegen'd before call sites.
5. **Codegen skip** (`CgVisitor::visit(FuncDefAST*)`): if `is_generic()`, return immediately.

### Unification (`unify(pattern, concrete, bindings)`)
- `TypeParam` vs concrete → bind (with occurs check and consistency check)
- Same `TypeKind` → recurse into children (Pointer pointee, Array element+size, Function params+return)
- Different `TypeKind` → return false

### Substitution (`substitute(type, bindings)`)
- `TypeParam` → look up in bindings, return concrete type
- Compound types → recurse into children
- Concrete types → return as-is

### Monomorphizer (`include/typecheck/Monomorphizer.h`)
- `Monomorphizer::instantiate(generic, mangled_name, bindings)` → deep-clones `FuncDefAST`
- Replaces `SimpleTypeExprAST` names according to bindings (e.g. `"T"` → `"i32"`)
- Sets cloned prototype's `functionName` to `mangled_name`, leaves `type_params` empty

### Limitations
- Generic functions only (not records/structs)
- Type parameters only (no const generics — array sizes must be concrete)
- No partial application of generic functions (all args must be provided)

## Adding a New AST Node to Type Checker
1. Add `synthesize()` declaration in `BiTypeChecker.h`
2. Add `preorder_walk()` and `postorder_walk()` declarations in `BiTypeChecker.h`
3. Implement all three in `BiTypeChecker.cpp`
