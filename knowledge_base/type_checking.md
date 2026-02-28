# Type Checking Patterns

## Type System (`include/typecheck/Types.h`)

| `TypeKind` | `TypeData` variant | Factory |
|---|---|---|
| `I32_t`, `I64_t`, `F64_t`, `Unit`, `Bool`, `Char`, `String`, `Never`, `NonExistent`, `Poisoned` | `std::monostate` | `Type::I32_t()`, etc. |
| `Integer`, `Flt` | `std::monostate` | Polymorphic literals — default to `I32_t`/`F64_t` via `default_polymorphic_type()` |
| `Function` | `FunctionType` | `Type::Function(params)` |
| `Pointer` | `PointerType` (`shared_ptr<Type>` — forward-decl needs indirection) | `Type::Pointer(pointee)` |
| `Array` | `ArrayType` | `Type::Array(elem, size)` |
| `Struct` | `StructType` | `Type::Struct(name, members)` |
| `Enum` | `EnumType` | `Type::Enum(name, variants)` |
| `Tuple` | `TupleType` | `Type::Tuple(element_types)` |
| `TypeParam` | `std::string` | `Type::TypeParam(name)` |

- `TypeData` = `std::variant<FunctionType, PointerType, ArrayType, StructType, EnumType, TupleType, std::string, std::monostate>`
- `PointerType` uses `shared_ptr<Type>` (forward-decl needs indirection); `TupleType` uses `shared_ptr<vector<Type>>` (same reason); `FunctionType`/`ArrayType` use `vector<Type>` (heap-backed, no forward-decl issue)
- Type's copy/move constructors/assignment/destructor must be declared in header but defaulted in `Types.cpp` to break circular variant instantiation
- `operator==` compares `type_kind` + `type_data` only — ignores `is_mutable`. Use `compatible_to_from(to, from)` for directional checks; rejects `immut → mut` for non-primitives. `is_literal()` types bypass this (always by-value).
- Immutable by default (`let x`); `let mut` for mutable locals, `mut` for mutable params. Set on `Type::is_mutable` during `synthesize(VarDefAST*)`/`synthesize(TypedVarAST*)`. Assignment to immutable LHS → error at `=` token.

## BiTypeChecker (`include/typecheck/BiTypeChecker.h`)
- Bidirectional: `synthesize()` → types bottom-up, `postorder_walk()` → consistency top-down
- Two lexical stacks: `id_to_type` (variable/function names), `typename_to_type` (type names); built-ins (i32/i64/f64/bool/char/unit) registered in `enter_new_scope()`
- All lookups use `QualifiedName::mangled()`, all errors use `.display()`
- **Enum variant invariant**: all variant calls arrive pre-qualified by scope generator. Type checker does NOT resolve unqualified variants.

### `resolve_type_expr(TypeExprAST*)`

| Input | Resolution |
|---|---|
| `nullptr` | `Type::NonExistent()` |
| `SimpleTypeExprAST` | check unresolved → lookup `.mangled()` in `typename_to_type` |
| `Pointer/Array/FunctionTypeExprAST` | recursive resolve → wrap in corresponding `Type::` factory |
| `TupleTypeExprAST` | recursive resolve each element → `Type::Tuple(element_types)` |
| `GenericTypeExprAST` | resolve type args → lookup `generic_enum_defs` → validate count → `Monomorphizer::instantiate_enum()` → register; unresolved type params defer |

### Literal Synthesis
- Integer (no decimal) → `Integer`; with decimal → `Flt` — defaulted when no context narrows
- Number suffixes (`i32`/`i64`/`f64`): extract suffix, strip from `ast->number`, set type; invalid → abort
- `StringExprAST` → `ptr<char>`, `CharExprAST` → `Char`
- `main`: must return `i32`, take 0 or 2 params (`(i32, ptr<ptr<char>>)`)

### Scope Lookup Helpers

| Helper | Scope | On failure |
|---|---|---|
| `get_type_from_id(str)` | current only | abort |
| `get_type_from_id_parent(str)` | parent only | abort |
| `try_get_callee_type(str)` | recursive (all ancestors) | `nullopt` |

`synthesize(VariableExprAST*)` uses `recursive_get_from_name()` for ancestor lookup.

## Partial Application & First-Class Functions
- Partial detection in `synthesize(CallExprAST*)`: `args.size() < params.size()` → `is_partial = true`, type = `Function(remaining_params..., ret)`
- Full call → return type. `visit(CallExprAST*)` uses `try_get_callee_type()` for all-scope lookup.

## Alloc/Free
- `alloc<T>(count)`: resolve `T` via `resolve_type_expr`, count must be integer → `ptr<T>` (`is_linear = true`)
- `free(expr)`: operand must be `ptr<T>` → `unit`

## Enum Types
- `EnumType`: `QualifiedName` + vector of `VariantInfo`; nominal equality (by name)
- `variant_constructors` map: `variant_name → (enum_type, variant_index)` — populated in `visit(EnumDefAST*)`
- **Construction** (`synthesize(CallExprAST*)`): qualified path → lookup module in `typename_to_type` → resolve variant; fallback → `variant_constructors`. Validates args, sets `is_enum_constructor`/`enum_variant_index`.
- **Unit variants** (`synthesize(VariableExprAST*)`): name not in `id_to_type` → try `variant_constructors`; empty payload → `is_enum_unit_variant`; has payload → abort
- **Generic enums**: `generic_enum_defs` → instantiated via `Monomorphizer::instantiate_enum()` → tracked in `instantiated_enums`/`monomorphized_enum_defs`

## Case Expressions / Pattern Matching
- Scrutinee must be enum. Each arm opens new scope with payload bindings in `id_to_type`.
- Wildcard `_` matches any variant; non-wildcard: `et.get_variant_index()` → validate binding count → set `variant_index`
- **Arm type unification**: starts `Never`; adopt first non-Never; skip Never arms; `compatible_to_from` bidirectional (same as `IfExprAST`)
- **Exhaustiveness**: all variants covered or wildcard present; reports missing names

## While Expressions
Condition must be `bool` (skip if `Poisoned`), result always `unit`.

## Arithmetic Operator Dispatch

| Op | Method | Built-in instances |
|---|---|---|
| `+` | `Add::add` | i32, i64, f64, char |
| `-` | `Sub::sub` | i32, i64, f64 |
| `*` | `Mul::mul` | i32, i64, f64 |
| `/` | `Div::div` | i32, i64, f64 |
| `%` | `Mod::mod` | i32, i64 |

`synthesize_binary_operator()`: lookup `ClassName__lhs_type` in `type_class_instances` → set `resolved_op_method` (e.g. `Add__i32__add`) → return `lhs_type`. Built-in instances have no source bodies — codegen emits inline ops.

## Three-Pass Registration (`visit(ProgramAST*)`)
1. **Types + typeclasses**: structs, enums, **type aliases**, typeclass decls/instances, builtin ops → type maps, `variant_constructors`, typeclass data. Type aliases: `resolve_type_expr()` on the alias's type expr, register result in `typename_to_type`.
2. **Function signatures**: `pre_register_function()` for func/extern/instance-methods → mutual recursion. Resolves types via `resolve_type_expr()` directly (not `accept_synthesis`). Generics → `generic_func_defs` instead.
3. **Full type checking**: all definitions (structs/enums/type aliases skip if already visited)

## Typeclasses
- `TypeClassInfo`: `name`, `type_param`, `methods`. `TypeClassInstanceInfo`: `class_name`, `concrete_type`, `method_mangled_names`.
- Maps: `type_class_defs` (name → info), `type_class_instances` (`"Class__Type"` → info), `method_to_class` (method → class)
- **Mangling**: `method` → `Class__Type__method` (e.g. `Sized__i32__sizeof`) — becomes the codegen function name
- `register_typeclass_decl()`: temp scope with `type_param` as `TypeParam`, synthesize prototypes, populate `method_to_class`
- `register_typeclass_instance()`: resolve concrete type, validate class, mangle names, update prototypes
- **Dispatch** (`synthesize(CallExprAST*)` with `explicit_type_args`): lookup in `method_to_class` → resolve type arg → find instance → set `resolved_generic_name` + `is_typeclass_call`; not found → fall through to generic handling
- Monomorphizing `print_size<T>` for `i32` resolves inner `sizeof<T>()` → `Sized__i32__sizeof`

## Monomorphized Generics

### Flow
1. **Registration** (`visit(FuncDefAST*)`): register `type_params` as `TypeParam` → if generic, store in `generic_func_defs`, skip body
2. **Generic call** (`synthesize_generic_call()`): check `generic_func_defs` → bind explicit type args or `unify()` to infer → mangled name → `substitute()` return type
3. **Instantiation** (`visit(CallExprAST*)`): `Monomorphizer::instantiate()` deep-clones AST → `GeneralSemanticsVisitor` → type-check clone → `monomorphized_defs`
4. **Injection**: `monomorphized_defs` → front of `DefinitionVec`
5. **Codegen**: `is_generic()` → skip

### Unification / Substitution
- `unify(pattern, concrete, bindings)`: `TypeParam` → bind (occurs + consistency check); same kind → recurse; different → false
- `substitute(type, bindings)`: `TypeParam` → lookup; compound → recurse; concrete → as-is

### Limitations
Generic functions and enums only (not structs). No const generics, no partial application of generics, no generic `reuse`.

### Type Param Shadowing
Type params shadow outer types — e.g. a struct named `T` is hidden by `<T>` inside a generic function. `PrototypeAST::type_params` is populated during **parsing**; type checker registers them in `typename_to_type` before visiting the body.

## Edge Cases
- **Polymorphic literal binops**: both operands polymorphic + same kind → skip dispatch, keep polymorphic. Excluded for comparison/logical ops (must return `Bool`).
- **Compound comparison guards**: Array/Pointer only allow `==`/`!=`; others → error + `Poisoned`.

## Adding a New Type Checklist
1. Add to `TypeKind` enum in `Types.h`
2. Add class to `TypeData` variant if compound
3. Add factory method on `Type`
4. Add `to_string()` case
5. Register in `enter_new_scope()` if named
6. Handle all `switch(type_kind)` (`-Wswitch` enforces)
7. Add case to `TypeConverter::get_type()` and `get_cmp_func()`

## Adding a New AST Node to Type Checker
1. Add `synthesize()` in `BiTypeChecker.h`
2. Add `preorder_walk()` and `postorder_walk()` in `BiTypeChecker.h`
3. Implement all three in `BiTypeChecker.cpp`

## Linear Type Checker (`src/typecheck/LinearTypeChecker.cpp`)

Separate pass after BiTypeChecker — enforces that `'ptr<T>` (heap-allocated) pointers are consumed exactly once.

### Dispatch (`check_stmt`)
Handles: `VarDefAST`, `BinaryExprAST`, `CallExprAST`, `FreeExprAST`, `ReturnExprAST`, `IfExprAST`, `WhileExprAST`, `CaseExprAST`, `StructLiteralExprAST`, `ArrayLiteralExprAST`, `TupleLiteralExprAST`, `DerefExprAST`.

### Consumption Rules
- `free(p)`, `let q = p` (move), `return p` (ownership transfer), passing as linear param, moving into struct field or array element
- NOT consumption: `*p` (deref), comparison, struct field access
- **Deref-after-consume**: `check_deref()` verifies the operand hasn't been consumed before dereferencing — prevents use-after-free (e.g. `free(p); *p` errors)

### Struct/Array Literal Consumption
- `check_struct_literal()`: walks each field value; if a `VariableExprAST` references a linear var, consumes it (moved into struct)
- `check_array_literal()`: walks each element; same pattern
- `'ptr<T>` syntax works everywhere `ParseTypeExpr()` is called, including struct fields (`data: 'ptr<i32>`) and array element types (`['ptr<i32>;1]`). `compatible_to_from()` rejects `'ptr<T>` → `ptr<T>` mismatches, so field/element type annotations must match linearity.

### Branch Consistency
All branches of `if`/`case` must agree — either all consume a linear var or none do.

### Loop Restriction
Outer linear vars cannot be consumed inside loops (would be consumed multiple times).
