# Type System — BiTypeChecker, Linear Types, Monomorphization

## Core Types
- Scalars: `I32_t`, `I64_t`, `U32_t`, `U64_t`, `F64_t`, `F32_t`, `Bool`, `Char`, `Unit`, `String`
- Compounds: `Function`, `Pointer`, `Array`, `Struct`, `Enum`, `Tuple`
- Pseudo: `Never`, `NonExistent`, `Poisoned`, `Integer` (polymorphic → default i32), `Flt` (→ default f64), `TypeParam`, `Generic`
- `PointerType` uses `shared_ptr<Type>` (forward-decl needs indirection); copy/move ctors declared in header, defaulted in .cpp
- `operator==` compares kind + data only (ignores mutability). Use `compatible_to_from(to, from)` for directional checks
- Immutable by default; `let mut` / `mut` param for mutable. `is_literal()` types bypass mutability checks

## ASTProperties
- Side table keyed by `NodeId` (auto-incrementing per `AstBase`)
- `CallProps`: callee_func_type, is_partial, resolved_name, type_bindings, is_typeclass_call, is_enum_constructor, enum_variant_index
- `BinaryProps`: resolved_op_method
- `VariableProps`: is_enum_unit_variant, enum_variant_index
- Writers (BiTypeChecker): `props_.call(id).field = val`; Readers (codegen): `props_.call(id)->field`

## BiTypeChecker
- Bidirectional: `synthesize()` → bottom-up, `postorder_walk()` → top-down consistency
- Two stacks: `id_to_type` (variables), `typename_to_type` (type names)
- Three-pass registration: 1) types + typeclasses + aliases 2) function signatures 3) full checking

### Literals
- Integer (no decimal) → `Integer`; with decimal → `Flt`
- Suffixes: `i32`/`i64`/`u32`/`u64`/`f64`/`f32`
- String → `ptr<char>`, Char → `Char`
- `main`: must return `i32`, take 0 or 2 params `(i32, ptr<ptr<char>>)`

### Operators
- **Arithmetic** (`+`/`-`/`*`/`/`/`%`): dispatch via typeclass instances (`Add<i32>::add` etc.); sets `resolved_op_method` on `BinaryProps`
- **Comparison** (`==`/`!=`/`<`/`>`/`<=`/`>=`): return `Bool`. Arrays/Pointers: only `==`/`!=` allowed
- **Logical** (`&&`/`||`): return LHS type (NOT Bool), no typeclass dispatch
- **Bitwise** (`&`/`|`/`^`/`<<`/`>>`): valid on integers and integer-backed enums, return operand type
- **Assignment** (`=`): returns `Unit`, checks LHS mutability
- **Unary negation** (`-`): rejects unsigned types and non-numeric types
- Polymorphic literal binops: both polymorphic + same kind → skip dispatch, keep polymorphic

### Indexing
- `arr[i]` → element type; static bounds checking for constant indices
- `ptr<T>[i]` → `T`; no bounds checking
- `len(arr)` → `i32`; `dim(arr)` → tuple of `i32` per nesting level

### Enums
- `EnumType`: QualifiedName + VariantInfo vector; nominal equality
- `variant_constructors` map: variant_name → (enum_type, index)
- Construction: qualified path → `get_typename_type(get_qualifier())` → resolve variant
- Unit variants via `synthesize(VariableExprAST*)` → `variant_constructors` fallback
- **Integer-backed**: backing type (i32/i64/u32/u64 only), discriminant uniqueness, no mixing with payloads, type lattice edge to backing type

### Case Expressions
- **Enum mode**: scrutinee must be enum, each arm opens scope with payload bindings, exhaustiveness checking
- **Literal mode**: integer/bool/char patterns. Bool needs both values or wildcard; int/char need wildcard. Float rejected. Duplicates detected.
- Arm type unification: starts `Never`; adopt first non-Never; bidirectional compat

### Variadic Functions
- Extern only (`...`); require at least N fixed params; no per-arg checking beyond fixed params

### Kernels
- `synthesize_kernel_map`: input must be Array, lambda 1-param matching element type, result = Array(lambda_ret, input_size)
- `synthesize_kernel_reduce`: input Array, operator `+`/`-`/`*`/`/`, concrete numeric element type, identity compat with element, result = element type

## Typeclasses
- `TypeClassInfo`: name, type_param, methods. Instances keyed by `MonomorphizedKey`
- Mangling: `MonomorphizedKey::to_typeclass_name(method)` → `Add<i32>::add`
- Dispatch supports both `sizeof<i32>()` (unqualified) and `Add<i32>::add(x,y)` (qualified)
- Both set `resolved_name` + `is_typeclass_call` on `CallProps`

## Monomorphized Generics
- Generic call → `unify()` type args → `substitute()` return type → `Monomorphizer::instantiate()` deep-clones AST → type-check clone → inject at front
- `unify(pattern, concrete, bindings)`: TypeParam → bind; compound → recurse
- `substitute(type, bindings)`: TypeParam → lookup; compound → recurse
- **Invariant**: `resolve_type_expr` with concrete bindings must be used (not just `substitute()`) for generic struct returns — triggers proper instantiation
- Limitations: generic functions and enums only (not structs directly). No const generics.

## Linear Type Checker (separate pass after BiTypeChecker)
- Enforces `'ptr<T>` consumed exactly once

### Consumption
- `free(p)`, `let q = p` (move), `return p`, passing as linear param, moving into struct/array/tuple field
- NOT consumption: `*p` (deref), comparison, field access

### Wrapper Types (structs/tuples/arrays with linear fields)
- Tracked via `VarInfo::children` (per-field recursive tracking)
- `containsLinear()` recursively checks for linear sub-components
- `check_children_consumed()` at scope exit for each wrapper

### Branch Consistency
- All branches of if/case must agree — either all consume a linear var or none do
- Must check children, not just top-level vars

### Loop Restriction
- Outer linear vars cannot be consumed inside loops

### Known Limitations
- Array elements: single `"*"` tracking, not per-element
- Nested struct fields: only single-level `FieldAccessExprAST`
- Enum payloads: not yet tracked
- Whole-struct move after partial child consume: not yet rejected

## Incompatibility Hints
- `incompatibility_hint(expected, actual)` returns note explaining why types don't match
- Cases: linearity mismatch, mutability, signed/unsigned mix, tuple arity mismatch
