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
- `CallProps`: callee_func_type, is_partial, resolved_name, type_bindings, is_typeclass_call, is_enum_constructor, enum_variant_index. `callee_func_type` is the signature the call goes to (a generic's or an instance's, as `resolve_generic_signature` resolves it: `substitute()` would leave a generic struct named as the generic, `Vec<T>`), set for every call to a function, normal, generic or typeclass; MIR lowering reads it to tell a lent argument from a moved one
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
- **Comparison** (`==`/`!=`/`<`/`>`/`<=`/`>=`): dispatch like arithmetic, through `Equal`/`NotEqual`/`Less`/`Greater`/`LessEqual`/`GreaterEqual` (`kOperatorClasses`), result `Bool`. Built in (`is_builtin_instance`): ordering for numbers (untyped literals too) and char; equality also for bool, pointers, arrays of those, and enums whose payloads have it (`is_builtin_equality`): same variant and equal payloads (`emitEnumEquality` compares tags, then selects the active variant's payload comparison, since other variants' bytes are unwritten; integer-backed enums compare as integers). No enum order. Structs and tuples need a user instance. Codegen: `emitComparison` for built-ins (operators and calls by name); otherwise the instance is called
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

### Indexing
- `arr[i]` → element type; static bounds checking for constant indices
- Any other `x[i]` goes through `Indexer<T, E>` (`synthesize_indexer`): `indexer_instances_for` finds the instances, concrete or from a generic one, whose collection type is `x`'s, and exactly one must apply. The index must be `i64`, `E` must own nothing, and `IndexProps::indexer_method` names the method. The method takes `ptr<T>`, so `x` is lent to it, and codegen calls the method with `x`'s address (`emitIndexerCall`; a temporary is spilled). `x[i] = v` is rejected in `check_assign_target`. `vec` has `instance<T> Indexer<Vec<T>, T>`
- `ptr<T>[i]` → `T`; no bounds checking. Only allowed in a `let unsafe` function (`check_unsafe_allowed`, using `ctx().enclosing_function`); `PrototypeAST::is_unsafe` is copied to monomorphized instances. Calling an unsafe function needs no marker. Calling `reuse` is not yet restricted (TODO at `visit(ExternAST *)`, waiting on a printf rework)
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
- Generic instances (`instance<T> Indexer<Vec<T>, T>`): `register_typeclass_instance` resolves their types with the parameters as `TypeParam`s and keeps them in `generic_instances` by class, not in `type_class_instances`. `has_instance(key)` falls back to `instantiate_generic_instance`, which unifies each pattern with the key, copies the instance (`Monomorphizer::instantiate_instance`, marked `is_instantiation`), registers it under the key and type-checks its methods. Keys with open or literal types are never instantiated. Copies are injected into the program like generic function copies and emitted with internal linkage; the generic instance itself is skipped by pre-registration, `visit`, the checks after typing and codegen. Overlap (`overlaps_registered_instance`) is two-sided unification (`unifiable`) with this instance's parameters renamed apart; built-in instances are not checked and win for their types
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

## Linear values
- `'buf<T>` (`Type::Buffer`, a `PointerType` with `is_buffer()`) is a linear buffer from `alloc_buf<T>(n)`: a different type from `'ptr<T>`, lent to `ptr<T>` like it (`structurally_compatible`, `unify`), indexed in unsafe functions, never dereferenced; MIR doesn't follow its values
- That every `'ptr<T>`, and every value holding one, is consumed exactly once is checked on MIR: see `codegen.md`, "The ownership check"
- `Drop<T>` (prelude): what a value does when it ends; the instance takes the value and must use up what it owns. `instantiate_drops`, at the end of checking, copies a generic `Drop` instance for each struct or enum a generic instantiated (no source calls it: the drops MIR inserts do). `drop(x)` takes any type, a no-op for one that owns nothing
- `OpenTypeCheck` (after BiTypeChecker, once the generic copies are in) reports each type argument nothing decided, at the variant it starts from (`None` with nothing to say what `T` is): MIR has no type for one. BiTypeChecker reports it itself only where it needs the type to choose (`report_open_type`: an operator's instance, a case's patterns)
- `ViewPlacementCheck` (after OpenTypeCheck) reports a non-linear `ptr<T>` anywhere but the whole type of a parameter or local (a view of a view is fine): in a struct's field or an enum's payload at its declaration, in an extern's or function's signature, in a `let`'s type, in a struct, array or tuple value (which catches a generic instance holding one)
- The rest of what was the AST linear checker is on MIR: writing through a view, `&` of owned memory or of an owner, binding an owning value into a closure, a closure that bound a view leaving its function (`ViewCheck`, see `codegen.md`); a discarded temporary and a wildcard arm over an owning payload are reported by the ownership check as still owned, until drops (`plans/drops.md`) drop them

## Incompatibility Hints
- `incompatibility_hint(expected, actual)` returns note explaining why types don't match
- Cases: linearity mismatch, mutability, signed/unsigned mix, tuple arity mismatch
