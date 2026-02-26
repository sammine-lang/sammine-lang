# Lexer & Parser Patterns

## Lexer
- Keywords are recognized in `handleID()` in `src/lex/Lexer.cpp` — add string-to-token mapping there
- Token types defined in `include/lex/Token.h` — add to `TokenType` enum and `TokenMap`
- Existing operator tokens reused for pointer ops: `TokMUL` = `*` (deref), `TokAndLogical` = `&` (addr-of), `TokLESS`/`TokGREATER` = `<`/`>` (for `ptr<T>`)
- Keyword tokens for built-in functions: `TokAlloc` = `alloc`, `TokFree` = `free`, `TokMUT` = `mut`
- Keyword tokens for imports: `TokReuse` = `reuse`, `TokExport` = `export` (replaces old `TokExtern`)
- Keyword tokens for typeclasses: `TokTypeclass` = `typeclass`, `TokInstance` = `instance`
- Removed: `TokSizeOf` — `sizeof` is now a regular typeclass method call, not a keyword
- Number literals support type suffixes: `42i32`, `600851475143i64`, `3.14f64` — the lexer consumes any alphanumeric suffix after digits, stored as part of the `TokNum` lexeme

### Adding a New Operator Token Checklist
1. Add enum value to `TokenType` in `include/lex/Token.h`
2. Add `{TokFoo, "foo"}` to `TokenMap` in `include/lex/Token.h`
3. If comparison: add `tok_type == TokFoo` to `Token::is_comparison()`
4. Add lexer handler logic in `src/lex/Lexer.cpp` — multi-char operators need lookahead (e.g. `!=` checks `input[i+1] == '='` before falling back to `!`)
5. Add `{TokenType::TokFoo, precedence}` to `binopPrecedence` in `src/Parser.cpp`
6. Add codegen case to `TypeConverter::get_cmp_func()` or binary op handler as needed

## Parser
- Parser uses committed/non-committed error model: `SUCCESS`, `COMMITTED_NO_MORE_ERROR`, `COMMITTED_EMIT_MORE_ERROR`, `NONCOMMITTED`
- Return type: `p<T>` = `pair<unique_ptr<T>, ParserError>`
- `ParsePrimaryExpr()` tries parsers in order — unary operators (deref, addr-of) and keyword expressions (alloc, free) go before `ParseCallExpr` since they start with operator/keyword tokens, not TokID
- `ParseTypeExpr()` handles compound types recursively (e.g. `ptr<ptr<i32>>`)
- Type annotations parsed as `TypeExprAST` hierarchy (not strings), avoiding double-parsing
- `alloc(expr)` and `free(expr)` follow the `keyword(expr)` pattern: expect keyword, `(`, parse inner expression, expect `)`
- `ParseTypeClassDecl()` parses `typeclass Name<T> { method_sig; ... }` — method signatures are prototypes only (no bodies)
- `ParseTypeClassInstance()` parses `instance Name<ConcreteType> { let method() -> T { body } ... }` — method implementations are full `FuncDefAST`
- Top-level `ParseDefinition()` tries `ParseTypeClassDecl` and `ParseTypeClassInstance` alongside `ParseFuncDef`, `ParseStructDef`, etc.

### Explicit Type Parameter Parsing (`let f<T, U>(...)`)
- `ParsePrototype()` parses optional `<T, U, ...>` between function name and parameter list
- After consuming `TokID` (function name), checks for `TokLESS`; if found, parses comma-separated `TokID` names, consumes closing `>` via `consumeClosingAngleBracket()`
- Stores parsed names in `proto->type_params` (same field the type checker uses)
- Applies to all `ParsePrototype()` call sites: `let` functions, `reuse` declarations, typeclass methods

### Explicit Type Argument Parsing (`f<T, U>(args)`)
- `ParseCallExpr()` uses **speculative parsing with rollback** for `<TypeExpr, ...>` after an identifier
- After consuming `name`, if next token is `<`: mark rollback point, parse comma-separated `TypeExpr` list, check for `>` then `(`
- If all succeed → keep the `explicit_type_args` vector, discard rollback
- If any step fails → rollback token stream to before `<`, treat `<` as a comparison operator
- `consumeClosingAngleBracket()` handles the `>>` token splitting for nested generics (e.g. `f<ptr<i32>, f64>()`)

## QualifiedName (`include/util/QualifiedName.h`)
- `QualifiedName` struct carries module + name separately instead of eagerly mangling `m::add` → `math$add`
- Factory methods: `QualifiedName::local("add")`, `::qualified("math", "add")`, `::unresolved_qualified("x", "add")`
- `.mangled()` → `"math$add"` (for scope lookups, codegen), `.display()` → `"math::add"` (for error messages)
- `.is_unresolved()` → true when alias wasn't found in `alias_to_module` — enables "Module 'x' is not imported" errors
- Used by: `CallExprAST::functionName`, `StructLiteralExprAST::struct_name`, `SimpleTypeExprAST::name`
- NOT used by (always local definitions): `PrototypeAST::functionName`, `StructDefAST::struct_name`, `VariableExprAST::variableName`
- Parser always consumes `::` when seen after ID — never silently skips it for unknown aliases

## AST Nodes
- All AST nodes extend `AstBase` (via `ExprAST` or `DefinitionAST`) and implement `Visitable`
- Each node needs: `getTreeName()`, `accept_vis()`, `walk_with_preorder()`, `walk_with_postorder()`, `accept_synthesis()`
- `TypeExprAST` hierarchy (`SimpleTypeExprAST`, `PointerTypeExprAST`) is NOT part of the visitor pattern — resolved directly by type checker via `dynamic_cast`
- `VarDefAST` has `bool is_mutable` — set from `let mut` in `ParseVarDef()`
- `TypedVarAST` has `bool is_mutable` — set from `mut` prefix in `ParseTypedVar()` (for function params)
- `TypedVarAST` stores `unique_ptr<TypeExprAST> type_expr` (nullptr = no annotation)
- `PrototypeAST` stores `unique_ptr<TypeExprAST> return_type_expr` (nullptr = unit return)
- `TypeClassDeclAST` (DefinitionAST): `class_name`, `type_param` (string, e.g. "T"), `vector<unique_ptr<PrototypeAST>> methods`
- `TypeClassInstanceAST` (DefinitionAST): `class_name`, `concrete_type_expr` (TypeExprAST), `concrete_type` (resolved during typecheck), `vector<unique_ptr<FuncDefAST>> methods`
- `CallExprAST` extensions: `explicit_type_args` (vector of TypeExprAST for `f<i32>()` syntax), `is_typeclass_call` (bool, flags typeclass dispatch)

## Adding a New AST Node Checklist
1. Forward declare in `include/ast/AstDecl.h`
2. Define class in `include/ast/Ast.h` (with all visitor methods)
3. Add visitor methods to `AstBase.h` (`ASTVisitor` + `TypeCheckerVisitor`)
4. Add default `visit()` in `src/ast/Ast.cpp`
5. Add parser function in `Parser.h` and `Parser.cpp`
6. Wire into `ParsePrimaryExpr()` (for expressions)
