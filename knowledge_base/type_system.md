# Type System — BiTypeChecker, Linear Types, Monomorphization

## Core Types
- Scalars: `I32_t`, `I64_t`, `U32_t`, `U64_t`, `F64_t`, `F32_t`, `Bool`, `Char`, `Unit`, `String`
- Compounds: `Function`, `Pointer`, `Array`, `Struct`, `Enum`, `Tuple`
- Pseudo: `Never`, `NonExistent`, `Poisoned` (prints as `?poisoned(file:line)`, `sammine_util::kPoisonedMarker`; `Reportee::echoes_error` drops an error, warning or note naming one once an error is reported: it echoes that error, so no check need test for it), `Integer` (polymorphic → default i32), `Flt` (→ default f64), `TypeParam`, `Generic`
- `PointerType` uses `shared_ptr<Type>` (forward-decl needs indirection); copy/move ctors declared in header, defaulted in .cpp
- `operator==` compares kind + data only (ignores mutability). Use `compatible_to_from(to, from)` for directional checks
- All bindings are immutable. Blocks are scopes

## ASTProperties
- Side table keyed by `NodeId` (auto-incrementing per `AstBase`)
- `CallProps`: `callee` (a `Reference`: `Local`, `Function{name}`, `Variant{index}` or `Builtin{instance}`, set once by the call's resolver), callee_func_type, is_partial. `VariableProps::value` is a `Reference` too, and so are `BinaryProps::method`, `CastProps::method` and `IndexProps::method`, all from `method_reference(key, method)`. `callee_func_type` is the signature the call goes to (a generic's or an instance's, as `resolve_generic_signature` resolves it: `substitute()` would leave a generic struct named as the generic, `Vec<T>`), set for every call to a function, normal, generic or typeclass; MIR lowering reads it to tell a lent argument from a moved one
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
- A string literal → `string` (`TypeKind::String`, no payload): text that lives as long as the program, ending where C reads to. Its MIR type is a view of char (`MIRTypes`), so nothing below the checker tells it from one, and it is no view to the checker: it goes in fields and results. `Convert<string, ptr<char>>` and back are built in, one pointer (`lowerBuiltinInstance`); the way back is allowed only in an unsafe function (`synthesize(CastExprAST)`). A string passed to a `ptr<char>` parameter is lent as that view (`lendable_to_from`, the rule that lends an owned pointer), so one function takes a literal's chars and a buffer's. A `ptr<char>` is no string, so it has no `Format`. Char → `Char`
- `main`: must return `i32`, take 0 or 2 params `(i32, ptr<ptr<char>>)`

### Operators
- **Arithmetic** (`+`/`-`/`*`/`/`/`%`): dispatch via typeclass instances (`Add<i32>::Add` etc.); sets `BinaryProps::method`
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
- `ptr<T>[i]` → `T`; no bounds checking. Only allowed in a `let unsafe` function (`check_unsafe_allowed`, using `ctx().enclosing_function`); `PrototypeAST::is_unsafe` is copied to monomorphized instances. Calling an unsafe function needs no marker. Calling a C binding, or naming one as a value, needs one too (`check_c_use`, by `c_bindings`): nothing checks what C does. `print` is why programs needn't: it is built in
- `len(arr)` → `i32`; `dim(arr)` → tuple of `i32` per nesting level

### Enums
- `EnumType`: QualifiedName + VariantInfo vector; nominal equality
- Names (`sammine_util::QualifiedName`, `util/QualifiedName.h`, its functions at the end of `Types.h`): a path of parts, each with the types it is applied to (`vec::push<i32>` is [vec, push applied to (i32)]; `Drop<Vec<i32>>::Drop` is [Drop applied to (Vec<i32>), Drop]). A name is a key as it is (`==`, `QualifiedNameHash`): the checker's scopes (`id_to_type`, `typename_to_type`), `variant_constructors`, the generic registries, `NameResolver::definitions` and `ASTProperties`' symbols are keyed by it, and struct and enum types are equal when their names are. `mangled()` only shows a name, for a message, and `symbol()` is what a definition is linked as; nothing looks a name up by its text. The two are the two `NameForm`s of one spelling (`spelled`, on a name and on a `Type`): Written drops the module of a program's own and the prelude's names, Linked keeps every module, in the types a name is applied to as well, so two types written alike never share a symbol (`definitions::Hash<::Range<i32>>::Hash` is a program's own Range, `definitions::Hash<definitions::Range<i32>>::Hash` the prelude's). Every definition's name starts with its module, a program's own with the module with no name (`helper` is [, helper]: shown `helper`, linked `::helper`), so a value's name of one part is always a local. The parser puts no types in a name (they are written as type expressions, kept beside it as `explicit_type_args`); the checker makes applied names from a `MonomorphizedKey` (`generic_name`, `to_typeclass_name`). A local, a type parameter or a built-in type is the name of one identifier (`bare`)
- Type names are canonical: `NameResolver` names every struct/enum `module::Name` after its `defining_module`, in its own library and in importers alike, so instance symbols like `shapes::Area<shapes::Square>::Area` agree across modules. The type checker uses the AST name as the type name. Executables' own types keep bare names
- `variant_constructors` map: variant_name → (enum_type, index)
- Construction: qualified path → `get_typename_type(get_qualifier())` → resolve variant
- Unit variants via `synthesize(VariableExprAST*)` → `variant_constructors` fallback
- **Integer-backed**: backing type (i32/i64/u32/u64 only), discriminant uniqueness, no mixing with payloads, type lattice edge to backing type

### Case Expressions
- **Enum mode**: scrutinee must be enum, each arm opens scope with payload bindings, exhaustiveness checking
- **Literal mode**: integer/bool/char patterns. Bool needs both values or wildcard; int/char need wildcard. Float rejected. Duplicates detected.
- Arm type unification: starts `Never`; adopt first non-Never; bidirectional compat

### Variadic Functions
- Extern only (`...`); require at least N fixed params. Through `...` goes only what C and sammine pass alike (`c_passes`, the rule for a callback's signature too): numbers, bool, char, pointers, integer-backed enums; no address (`&x`), and a function only through a declared parameter. An open type is left to `OpenTypeCheck`

### Kernels
- `synthesize_kernel_map`: input must be Array, lambda 1-param matching element type, result = Array(lambda_ret, input_size)
- `synthesize_kernel_reduce`: input Array, operator `+`/`-`/`*`/`/`, concrete numeric element type, identity compat with element, result = element type

## Typeclasses
- A typeclass is a definition of its module: `TypeClassDeclAST::class_name` is a `QualifiedName` NameResolver names (`shapes::Area`, a program's by the module with no name, the prelude's `definitions::Add`, written `Add`), and an instance's `class_name` is resolved to it (`instance shapes::Area<T>`). `type_class_defs`, `generic_instances` and `method_to_class` are keyed by name. Instances keyed by `MonomorphizedKey`; `MonomorphizedKey::instance(cls, types)` is for the prelude's classes, and only those have built-in instances (`is_builtin_instance`)
- An instance is its class's module's to write, or the module's of one of its types (`owns_instance`, in `register_typeclass_instance` after the built-in check): the instance's `defining_module` against the class name's module and each type's (`module_of`: a struct's or enum's name's module, through pointers; none for a number, string, tuple, array or type parameter). So no two modules write one instance, which a program could then have only once
- Generic instances (`instance<T> Indexer<Vec<T>, T>`): `register_typeclass_instance` resolves their types with the parameters as `TypeParam`s and keeps them in `generic_instances` by class, not in `type_class_instances`. `has_instance(key)` falls back to `instantiate_generic_instance`, which unifies each pattern with the key, copies the instance (`Monomorphizer::instantiate_instance`, marked `is_instantiation`), registers it under the key and type-checks its methods. Keys with open or literal types are never instantiated. Copies are injected into the program like generic function copies and emitted with internal linkage; the generic instance itself is skipped by pre-registration, `visit`, the checks after typing and codegen. Overlap (`overlaps_registered_instance`) is two-sided unification (`unifiable`) with this instance's parameters renamed apart; built-in instances are not checked and win for their types
- An instance method's name: `MonomorphizedKey::to_typeclass_name(method)` → `Add<i32>::Add`, `shapes::Area<shapes::Square>::Area`
- Dispatch supports a method by its own name, `sizeof<i32>()`, and through its class, `Add<i32>::add(x,y)`. NameResolver declares each method beside its class (`m::show` for `m::Show`'s `show`; one named as its class is the class's name), so `show<i32>(x)` in the class's module and `m::show<i32>(x)` elsewhere resolve to it; `method_to_class` is keyed by that name. A module's own top-level name hides the prelude's of that name, a method's (`next`) as any other
- Both set `CallProps::callee` through `method_reference`
- Built-in instances (`Add<i32>` etc.) have no source: `is_builtin_instance()` in `typecheck/BuiltinInstances.h` decides them, and `kOperatorClasses` there maps each operator to its class. Look instances up with `has_instance()`, never `type_class_instances` directly. A call to one sets `CallProps::builtin_instance`, and codegen emits the instruction (`emitBuiltinInstanceCall`). Redefining one is an error

## Monomorphized Generics
- Generic call → `unify()` type args → `substitute()` return type → `Monomorphizer::instantiate()` deep-clones AST → type-check clone → inject at front
- `unify(pattern, concrete, bindings)`: TypeParam → bind; compound → recurse
- `substitute(type, bindings)`: TypeParam → lookup; compound → recurse
- **Invariant**: `resolve_type_expr` with concrete bindings must be used (not just `substitute()`) for generic struct returns — triggers proper instantiation
- Limitations: generic functions and enums only (not structs directly). No const generics.

## Linear values
- `'buf<T>` (`Type::Buffer`, a `PointerType` with `is_buffer()`) is a linear buffer from `alloc_buf<T>(n)`: a different type from `'ptr<T>`, lent to `ptr<T>` like it (`structurally_compatible`, `unify`), indexed in unsafe functions, never dereferenced (a `'ptr<T>` is dereferenced, never indexed: it holds one value); MIR doesn't follow its values
- That every `'ptr<T>`, and every value holding one, is consumed exactly once is checked on MIR: see `codegen.md`, "The ownership check"
- `Drop<T>` (prelude): what a value does when it ends; the instance takes the value and must use up what it owns. `instantiate_drops`, at the end of checking, copies a generic `Drop` instance for each struct or enum a generic instantiated (no source calls it: the drops MIR inserts do). `drop(x)` takes any type, a no-op for one that owns nothing
- `OpenTypeCheck` (after BiTypeChecker, once the generic copies are in) reports each type argument nothing decided, at the variant it starts from (`None` with nothing to say what `T` is): MIR has no type for one. BiTypeChecker reports it itself only where it needs the type to choose (`report_open_type`: an operator's instance, a case's patterns)
- `ViewPlacementCheck` (after OpenTypeCheck) reports a non-linear `ptr<T>` anywhere but the whole type of a parameter or local (a view of a view is fine): in a struct's field or an enum's payload at its declaration, in an extern's or function's signature, in a `let`'s type, in a struct, array or tuple value (which catches a generic instance holding one)
- `NeverValueCheck` (after ViewPlacementCheck) reports an expression typed `Never` (a `panic(...)`, an `if`/`case` whose branches all panic or return) whose value something uses: an argument, an operand, an element or field, a condition, a scrutinee, an index. MIR lowering has no value to give there. A statement, a branch's or block's value, a `let`'s value (it binds nothing: `lowerVarDef` lowers it and the block ends; `let (a, b) =` one gives each name its annotation's type or `Never`) and a `return`'s are fine
- Formatted output is one node, `FormatExprAST`, of five kinds: `print` (stdout), `eprint` (stderr), `write(f, ...)` (the Formatter `f`), `format` (nowhere: it gives the text, `('buf<char>, i64)`, which `str::from` makes a String of) and `panic` (stderr, then exit). Each is its own keyword; `format_index()` says which argument is the format. The format must be a `StringExprAST`, read once by `sammine_util::FormatString::parse` (`util/FormatString.h`: text, `{}`, `{.N}`, `{{`, `}}`) in the checker and kept in `FormatProps::format` for lowering. Each argument prints through `Format<T>` of its type (prelude; `kFormatClass`), which the checker asks `has_instance` for; lowering goes by the types alone. What the compiler prints is in `BuiltinInstances.h`: `builtin_format_directive` (numbers, bool, char, string, integer-backed enums → C's directive) and `format_parts` (a tuple's elements, an array's), which `is_builtin_instance`, `has_instance` (a tuple or array prints if its parts do; the error names the part that doesn't) and lowering all read. An untyped literal is settled first (`default_polymorphic_type`); an open type is left to `OpenTypeCheck`; `{.N}` on a built-in type that isn't a float is an error, anywhere else it is handed on. `Formatter` is a built-in type (`TypeKind::Formatter`), opaque to programs: in MIR a tuple of the C stream it writes to and the digits, an i32, fewer than none where none was said (`MIRTypes`). MIR lowering (`lowerFormat`, `writeValue`) gathers C's format and its arguments in a `FormatOutput` and writes them with `fprintf` (`flush`): a directive per built-in argument, a `%` of the text doubled, a bool as "true" or "false" (`boolText`), a tuple's parts between parentheses, an array's between brackets (the first inline, the rest in a loop); a program's instance is a flush, then a call of its method with the value and a Formatter built of the stream and the digits. Every destination is a C stream: `sammine_stdout()` and `sammine_stderr()`, and for format `sammine_text_open/stream/finish/take`, a memory stream, all in the prelude's C runtime (`stdlib/definitions_runtime.c`, declared by `declareRuntime`, linked with the prelude into every program). panic prefixes `file:line:col: panic: `, ends with a newline, then exit(1) and `ub.unreachable`; `MIRLowering::fail` does the same for the array bounds check: no drop is elaborated on its path, which joins no other. `Format<T>(x, f)` called by name, for a T the compiler prints, is `writeBuiltin`: the digits, known only then, go to printf beside a float (`%.*f`)
- The rest of what was the AST linear checker is on MIR: writing through a view, `&` of owned memory or of an owner, binding an owning value into a closure, a closure that bound a view leaving its function (`ViewCheck`, see `codegen.md`); a discarded temporary and a wildcard arm over an owning payload are reported by the ownership check as still owned, until drops (`plans/drops.md`) drop them

## Incompatibility Hints
- `incompatibility_hint(expected, actual)` returns note explaining why types don't match
- Cases: linearity mismatch, mutability, signed/unsigned mix, tuple arity mismatch
