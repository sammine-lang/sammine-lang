# Type System — BiTypeChecker, Linear Types, Monomorphization

## Core Types
- Scalars: `I32_t`, `I64_t`, `U32_t`, `U64_t`, `F64_t`, `F32_t`, `Bool`, `Char`, `Unit`, `String`
- Compounds: `Function`, `Pointer`, `Array`, `Struct`, `Enum`, `Tuple`
- Pseudo: `Never`, `NonExistent`, `Poisoned`, `Integer` (polymorphic → default i32), `Flt` (→ default f64), `TypeParam`, `Generic`
- `PointerType` uses `shared_ptr<Type>` (forward-decl needs indirection); copy/move ctors declared in header, defaulted in .cpp
- `operator==` compares kind + data only (ignores mutability). Use `compatible_to_from(to, from)` for directional checks
- All bindings are immutable. Blocks are scopes

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
- Pass 1 order-independence: declare every struct/enum (`declare_struct`/`declare_enum`, empty shared body) → expand aliases on demand (`resolve_pending_alias`, cycle → "Type alias 'T1' refers to itself through 'T2'") → fill bodies (`StructType::define`/`EnumType::define`) → alias variant constructors → typeclass decls → instances
- No self-referential types, even through `ptr`: `refers_to_itself` rejects a field/payload reaching its own type (walks pointers, arrays, tuples, functions, other structs/enums)

### Literals
- Integer (no decimal) → `Integer`; with decimal → `Flt`
- Suffixes: `i32`/`i64`/`u32`/`u64`/`f64`/`f32`
- String → `ptr<char>`, Char → `Char`
- `main`: must return `i32`, take 0 or 2 params `(i32, ptr<ptr<char>>)`

### Operators
- **Arithmetic** (`+`/`-`/`*`/`/`/`%`): dispatch via typeclass instances (`Add<i32>::add` etc.); sets `resolved_op_method` on `BinaryProps`
- **Comparison** (`==`/`!=`/`<`/`>`/`<=`/`>=`): dispatch like arithmetic, through `Equal`/`NotEqual`/`Less`/`Greater`/`LessEqual`/`GreaterEqual` (`kOperatorClasses`), result `Bool`. Built in (`is_builtin_instance`): ordering for numbers (untyped literals too) and char; equality also for bool, pointers, arrays of those (`is_builtin_equality`). Structs, tuples and enums need a user instance (no built-in enum equality: a payload only means something per variant). Codegen: `emitComparison` for built-ins (operators and calls by name); otherwise the instance is called
- **Logical** (`&&`/`||`): return LHS type (NOT Bool), no typeclass dispatch
- **Bitwise** (`&`/`|`/`^`/`<<`/`>>`): valid on integers and integer-backed enums, return operand type
- **Assignment** (`=`): returns `Unit`; `check_assign_target` follows the LHS through array elements and fields (`write_root`) and requires it to land behind a pointer (`*p`, `p[i]`)
- **Unary negation** (`-`): rejects unsigned types and non-numeric types
- Polymorphic literal binops: both polymorphic + same kind → skip dispatch, keep polymorphic

### Conversions (`as`)
- `CastExprAST` (`operand as T`), parsed by `ParseCastExpr` around every binary operand: tighter than binops, looser than prefix `-`/`*`/`&`
- Goes through `Convert<From, To>` (declared in `definitions.mn`); `CastProps::instance` holds the key
- Built in (`is_builtin_conversion`): number ↔ number, char ↔ integer. Codegen: `emitNumberConversion` (sext/zext by source signedness, trunc, `llvm.fpto[su]i.sat` so NaN → 0 and out of range clamps)
- A polymorphic literal operand takes `T` if compatible, else its default (i32/f64)
- Linear checker: operand is owned when its type contains linear types, else read

### Indexing
- `arr[i]` → element type; static bounds checking for constant indices
- `ptr<T>[i]` → `T`; no bounds checking
- `len(arr)` → `i32`; `dim(arr)` → tuple of `i32` per nesting level

### Enums
- `EnumType`: QualifiedName + VariantInfo vector; nominal equality
- Type names are canonical: `NameResolver` names every struct/enum `module::Name` after its `defining_module`, in its own library and in importers alike, so instance symbols like `Area<shapes::Square>::Area` agree across modules. The type checker uses the AST name as the type name. Executables' own types keep bare names
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
- Built-in instances (`Add<i32>` etc.) have no source: `is_builtin_instance()` in `typecheck/BuiltinInstances.h` decides them, and `kOperatorClasses` there maps each operator to its class. Look instances up with `has_instance()`, never `type_class_instances` directly. A call to one sets `CallProps::builtin_instance`, and codegen emits the instruction (`emitBuiltinInstanceCall`). Redefining one is an error

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

### Uses (`check_expr(expr, Use)`)
- Every child is visited with what its parent does: `Own`, `Read`, `Write`, `TakeAddress`
- A linear temporary (anything not a place or statement) is only legal where it is owned
- Writing through a non-linear `ptr`, or taking the address of owned memory, is decided in `check_through_pointer`
- Lending: a `'ptr` argument to a `ptr` parameter is a `Read`; lending and moving the same value in one call is an error

### Pointer Placement
- A non-linear `ptr<T>` may only be the whole type of a parameter or local (a view of a view is fine)
- Checked in `check_placement` for prototypes, externs, struct/enum defs, locals and literals

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
