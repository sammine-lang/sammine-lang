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
- `ASTContext`: tracks `in_imported_def`, `import_module`, `generic_type_params`

### Key behaviors
- Registers top-level defs (functions, externs, structs, enums, type aliases, kernels)
- Enum variants: always added to `variant_to_enum`; registered in scope only if not already there
- Functions registered in **parent** scope (visible at call-site, not inside own body)
- Unqualified variant calls rewritten to qualified form (e.g. `Red(1)` → `Color::Red(1)`)
- Case arm bodies NOT walked — payload bindings created by type checker
- Import qualification: `qualify_type_expr()` called in preorder for CallExprAST (explicit type args), AllocExprAST (type_arg), TypedVarAST (type_expr); `postorder_walk(StructLiteralExprAST*)` qualifies struct_name

## GeneralSemanticsVisitor (second semantic pass)
- Reserved identifiers: names containing `__` rejected (C interop mangling). Checked on: FuncDef, VarDef, StructDef, EnumDef, TypeAliasDef
- Implicit return insertion: unit-returning → appends `return ()`; value-returning → wraps last statement if it's an expression and not already a return
- TypeClassInstanceAST methods get implicit return wrapping
