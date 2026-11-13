# Frontend — Lexer, Parser, Semantic Analysis

## Lexer
- Reused tokens: `*` (deref/mul), `&` (addr-of/logical-and), `<`/`>` (generics/comparison)
- `TokTick` = `'` (linear pointer prefix `'ptr<T>`, `'buf<T>`; `buf` is an identifier, a type name only after the tick)
- `TokFloorDiv` (`/_`), `TokCeilDiv` (`/^`) — defined but not in precedence table yet
- `TokEllipsis` (`...`) — varargs and range syntax
- Number suffixes consumed as part of lexeme (`42i32`, `3.14f64`)
- A number right after a `.` (`TokenStream::ends_with(TokDot)`) is a tuple's index: its digits only, no fraction and no suffix, so `t.0.1` and `t.0.x` are field accesses one after the other. The type checker reads the whole lexeme as the index (`std::from_chars`), dropping nothing
- Escape sequences in strings/chars: `\n`, `\t`, `\r`, `\\`, `\'`, `\0`
- Token stream supports speculative parsing: `mark_rollback()`, `rollback()`, `split_current()` (for splitting `>>` into `>` + `>`)

## Parser
- Error model: `SUCCESS` / `FAILED` / `NONCOMMITTED`; `tryParsers<T>(Fns...)` returns first non-NONCOMMITTED
- `ParsePrimaryExpr()`: unary/keyword parsers first, then `ParseIdentifierExpr` (handles both calls and variables), then literals. Postfix `[index]` and `.field` via `parsePostfixOps()`
- `ParseDefinition()`: TypeClassDecl, TypeClassInstance, KernelDef, StructDef, EnumDef, ReuseDef, FuncDef
- Pipe `|>`: desugared to `f(x)` at parse time (precedence 1, lowest)
- `parseQualifiedNameTail(first_tok)`: greedy `::ID` chain consumer, used everywhere. Names are kept as written: the parser resolves nothing, `NameResolver` does. A name is its path alone: the types in `Option<i32>::Some` are returned as type expressions (`explicit_type_args`), which the type checker resolves

### Speculative Parsing
- `parseExplicitTypeArgsTail()`: tries `<Type, ...>` after a name; rollback if not type args
- `consumeClosingAngleBracket()`: splits `>>` → `>` + `>` and `>=` → `>` + `=`
- Struct literal vs block: lookahead in `ParseIdentifierExpr`

### Kernel Parsing
- `kernel name(params) -> T { kernel_exprs }` → `KernelDefAST`
- Body is `KernelBlockAST` (separate from `BlockAST`)
- Kernel expressions: `map(arr, lambda)` → `KernelMapExprAST`, `reduce(arr, op, identity)` → `KernelReduceExprAST`, number literals → `KernelNumberExprAST`
- `KernelExprAST` is NOT part of CPU visitor pattern — stub visitors

### Type Expressions
- Uses `ParseKind` enum discriminator (not RTTI); resolved via `dynamic_cast` in type checker
- `Simple`, `Pointer` (+ `is_linear`), `Array`, `Function`, `Tuple`, `Generic`

### Enum/Type Alias Disambiguation
- After `type Name =`: if next tokens are `ID |` or `ID(` → enum, else → type alias
- `type` keyword, not `enum` (TokEnum removed)

## ScopeGeneratorVisitor (first semantic pass)
- Its `LexicalStack<Location>` holds locals only: parameters, lets, case bindings, loop variables. Top-level names are the resolver's, per module
- `ASTContext`: tracks `module` (the `defining_module` of the definition being walked: whose code this is) and `type_params`, set by `enter_definition`

### Key behaviors
- `preorder_walk(ProgramAST)`: `resolver.use_program`, then every enum's variants into `variant_to_enum`
- A parameter may not take a name its definition's code already has (`resolver.defined_at`): another parameter, or a top-level name in its module's scope. An importer's names aren't, so `vec::push`'s `v` doesn't clash with a program's function `v`. A `let` may shadow
- Case arm bodies NOT walked — payload bindings created by type checker
- Typeclasses: `use_program` declares a class as any definition, and each of its methods beside it; `visit(TypeClassDeclAST)` resolves the types its method signatures are written with. Instance methods: the class an instance is of is resolved (`resolve_type_name`), and signatures and bodies are resolved in the instance's module; their names aren't registered (reached through the typeclass). Case patterns: a variant named through its enum is resolved (`resolve_variant`)

### Name resolution (`NameResolver`, semantics/NameResolver.h)
- The one place names are decided. `use_program` first names every definition (`qualify(name, defining_module)`: `M::name`, with no exception; an executable's by the module with no name, linked `::name`, and the prelude's by `definitions`; both are written and shown without the module) and gives each C binding its symbol, its C name, and a program's `main` the symbol `main`, in `ASTProperties::set_symbol`; then each reference is resolved below, a kernel's body included (`visit(KernelDefAST)`). ScopeGeneratorVisitor takes the ASTProperties to pass to it
- Every module has its own scope: `use_program` keeps, per module, its top-level names as written to their names (`top_levels`), and every definition by its name (`definitions`: module, exported, location). `top_level(M, n)` is the one lookup of a bare name: M's own, else one the prelude exports. A module defining a name twice, or one the prelude exports (a program's and the prelude's share unqualified names; a library's checked after), is reported there
- Every name position calls the resolver with `ctx()`: type exprs (`resolve_type`: params, lets, returns, alloc, cast, explicit type args, struct/enum members, aliases), struct literals (`resolve_type_name`), callees and variables (`resolve_value`). Adding a name position = one call, no logic
- Resolved in the module whose code it is, wherever it is compiled: an imported generic body sees only its module's names and the prelude's, so an importer's same-named definition can neither capture its names nor clash with its parameters
- Values: locals → own module, else the prelude's exports → variants (own module's enums first; `Red(1)` → `Color::Red(1)`) → as written. `X::N` with X not an import (unresolved) resolves X as a type: `Shape::Line` → `M::Shape::Line`
- Types: type params → builtins → own module, else the prelude's exports → as written
- Qualified paths (`resolve_head`): the first part is one of the writing module's import aliases → that module (`ProgramAST::module_aliases`, from each module's imports, filled by Compiler::parse and the import resolver), else the module's own type (`Shape::Line`); else "Module 'X' is not imported"
- `CallExprAST::written_name` keeps the callee as written (`h::double_it`) for error messages; `functionName` is what it resolved to

## GeneralSemanticsVisitor (second semantic pass)
- Reserved identifiers: names containing `__` rejected (C interop mangling). Checked on: FuncDef, VarDef, StructDef, EnumDef, TypeAliasDef
- No return is inserted: what a body returns when it runs off its end is `FuncDefAST::body_value()`, its last expression when the function writes a return type and that expression is a value (not a statement, not a `return`); otherwise `()`. The type checker checks it as a `return` (`check_returned`), MIR lowering returns it
