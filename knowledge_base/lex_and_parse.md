# Lexer & Parser Patterns

## Lexer
- Keywords are recognized in `handleID()` in `src/lex/Lexer.cpp` — add string-to-token mapping there
- Token types defined in `include/lex/Token.h` — add to `TokenType` enum and `TokenMap`
- Existing operator tokens reused for pointer ops: `TokMUL` = `*` (deref), `TokAndLogical` = `&` (addr-of), `TokLESS`/`TokGREATER` = `<`/`>` (for `ptr<T>`)
- Keyword tokens for built-in functions: `TokAlloc` = `alloc`, `TokFree` = `free`, `TokMUT` = `mut`
- Number literals support type suffixes: `42i32`, `600851475143i64`, `3.14f64` — the lexer consumes any alphanumeric suffix after digits, stored as part of the `TokNum` lexeme

## Parser
- Parser uses committed/non-committed error model: `SUCCESS`, `COMMITTED_NO_MORE_ERROR`, `COMMITTED_EMIT_MORE_ERROR`, `NONCOMMITTED`
- Return type: `p<T>` = `pair<unique_ptr<T>, ParserError>`
- `ParsePrimaryExpr()` tries parsers in order — unary operators (deref, addr-of) and keyword expressions (alloc, free) go before `ParseCallExpr` since they start with operator/keyword tokens, not TokID
- `ParseTypeExpr()` handles compound types recursively (e.g. `ptr<ptr<i32>>`)
- Type annotations parsed as `TypeExprAST` hierarchy (not strings), avoiding double-parsing
- `alloc(expr)` and `free(expr)` follow the `keyword(expr)` pattern: expect keyword, `(`, parse inner expression, expect `)`

## AST Nodes
- All AST nodes extend `AstBase` (via `ExprAST` or `DefinitionAST`) and implement `Visitable`
- Each node needs: `getTreeName()`, `accept_vis()`, `walk_with_preorder()`, `walk_with_postorder()`, `accept_synthesis()`
- `TypeExprAST` hierarchy (`SimpleTypeExprAST`, `PointerTypeExprAST`) is NOT part of the visitor pattern — resolved directly by type checker via `dynamic_cast`
- `VarDefAST` has `bool is_mutable` — set from `let mut` in `ParseVarDef()`
- `TypedVarAST` has `bool is_mutable` — set from `mut` prefix in `ParseTypedVar()` (for function params)
- `TypedVarAST` stores `unique_ptr<TypeExprAST> type_expr` (nullptr = no annotation)
- `PrototypeAST` stores `unique_ptr<TypeExprAST> return_type_expr` (nullptr = unit return)

## Adding a New AST Node Checklist
1. Forward declare in `include/ast/AstDecl.h`
2. Define class in `include/ast/Ast.h` (with all visitor methods)
3. Add visitor methods to `AstBase.h` (`ASTVisitor` + `TypeCheckerVisitor`)
4. Add default `visit()` in `src/ast/Ast.cpp`
5. Add parser function in `Parser.h` and `Parser.cpp`
6. Wire into `ParsePrimaryExpr()` (for expressions)
