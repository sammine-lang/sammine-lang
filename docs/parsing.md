# Parsing.md : High-level documentation describing parsing protocols and error handling

Hi there :) enjoy your stay
## Parsing protocol

Parsing expression, and binary expression.

## Error recovery

In principle, we say that if a Parse*() result is nullptr, then we haven't
committed to calling that Parse*() function yet. 

If a Parse*() commits (returns SUCCESS or FAILED status), then it either parsed successfully or
failed after commitment. Error messages are reported immediately to an external Reporter via `imm_error()`.

If a Parse*() commits and at a certain time cannot match against a TokenType,
it will suppose that it has matched successfully against that token but reports an error.

There are two kind of Parse* that can be recoverable

### Statement Parsing
Simple statement parsing: if we encounter an error in matching a token, we exhaust the token stream until we hit a semicolon. Then we return that statement as if we have parsed that statement.
Of course we need to account for error reporting at appropriate places.

If statement parsing: if we encounter an error in matching a token, we exhaust the token stream until we hit a right curly bracket. Then we return that statement as if we have parsed that statement.
Of course we need to account for error reporting at appropriate places.

This means that for Simple statement, if a ParseExpr* commits and fails, it can push back to
error_msg vector and then promptly return a partially completed AST with error = True. If simple statement has loop all over the function vector and
cannot have anything non-null or it receives partial error, it exhausts until semicolon. Else if it has sth non-null. then it expects a semicolon, 
if it doesn't have it, then it errors and also exhausts until semicolon.

  
### Definition Parsing

Same as statement parsing, since a variable definition ends in a semicolon and the other two
ends in a right curly, you delegate the exhaustion responsbilities to each of them.

If at first we can't even match them against the first token in each rule, we return nullptr for
all of them, and then since ParseProgram fails at this point, we stop computation with exhaust_until_EOF.
Since a statement can be a simple statement which ends in a semicolon or an if
- Definition parsing
  - StructDef
  - EnumDef / TypeAliasDef
  - FuncDef / ExternDef
  - TypeClassDecl
  - TypeClassInstance

TLDR: unless the current parse is at simple_stmt or return_stmt or if_stmt or one of the ParseDef, if we errors on a match, immediately
exhausts until EOF with an error message.

- If we error at simple_stmt or return_stmt, exhausts until semicolon.
- If we error at if_stmt, exhausts until right curly (via ParseBlock).
- If we error at VarDef, exhausts until semicolon.
- If we error at FuncDef or StructDef, exhausts until right curly.
- If we error at EnumDef or TypeAliasDef, exhausts until semicolon.

Error means we have committed at that level of Parse() but failed to match against a token.