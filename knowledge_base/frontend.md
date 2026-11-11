# Frontend — Lexer, Parser, Semantic Analysis

## Lexer
- Reused tokens: `*` (deref/mul), `&` (addr-of/logical-and), `<`/`>` (generics/comparison)
- `TokTick` = `'` (linear pointer prefix `'ptr<T>`)
- `TokFloorDiv` (`/_`), `TokCeilDiv` (`/^`) — defined but not in precedence table yet
- `TokEllipsis` (`...`) — varargs and range syntax
- Number suffixes consumed as part of lexeme (`42i32`, `3.14f64`)
- Escape sequences in strings/chars: `\n`, `\t`, `\r`, `\\`, `\'`, `\0`
- Token stream supports speculative parsing: `mark_rollback()`, `rollback()`, `split_current()` (for splitting `>>` into `>` + `>`)

## Parser
- Error model: `SUCCESS` / `FAILED` / `NONCOMMITTED`; `tryParsers<T>(Fns...)` returns first non-NONCOMMITTED
- `ParsePrimaryExpr()`: unary/keyword parsers first, then `ParseIdentifierExpr` (handles both calls and variables), then literals. Postfix `[index]` and `.field` via `parsePostfixOps()`
- `ParseDefinition()`: TypeClassDecl, TypeClassInstance, KernelDef, StructDef, EnumDef, ReuseDef, FuncDef
- Pipe `|>`: desugared to `f(x)` at parse time (precedence 1, lowest)
- `parseQualifiedNameTail(first_tok, resolve_alias=true)`: greedy `::ID` chain consumer, used everywhere

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
- Builds lexical symbol table via `LexicalStack<Location>`
- `ASTContext`: tracks `module` (the `defining_module` of the definition being walked: whose code this is) and `type_params`, set by `enter_definition`

### Key behaviors
- Registers top-level defs (functions, externs, structs, enums, type aliases, kernels)
- Enum variants: always added to `variant_to_enum`; registered in scope only if not already there
- Functions registered in **parent** scope (visible at call-site, not inside own body)
- Case arm bodies NOT walked — payload bindings created by type checker

### Name resolution (`NameResolver`, semantics/NameResolver.h)
- Every name position calls the resolver with `ctx()`: type exprs (`resolve_type`: params, lets, returns, alloc, cast, explicit type args, struct/enum members, aliases), struct literals (`resolve_type_name`), callees and variables (`resolve_value`). Adding a name position = one call, no logic
- Resolved in the module whose code it is: its own definition first (`own_definition`: `M::X` when M was imported), so an imported generic body finds its module's names and an importer's same-named definition can't capture them
- Values: locals → own module → variants (own module's enums first; `Red(1)` → `Color::Red(1)`) → as written. `X::N` with X not an import (unresolved) resolves X as a type: `Shape::Line` → `M::Shape::Line`
- Types: type params → builtins → own module → as written

## GeneralSemanticsVisitor (second semantic pass)
- Reserved identifiers: names containing `__` rejected (C interop mangling). Checked on: FuncDef, VarDef, StructDef, EnumDef, TypeAliasDef
- Implicit return insertion: unit-returning → appends `return ()`; value-returning → wraps last statement if it's an expression and not already a return
- TypeClassInstanceAST methods get implicit return wrapping
