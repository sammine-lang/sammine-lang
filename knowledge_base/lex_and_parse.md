# Lexer & Parser Patterns

## Lexer

Keywords recognized in `handleID()` (`src/lex/Lexer.cpp`). Token types defined in `include/lex/Token.h` (`TokenType` enum + `TokenMap`).

| Token(s) | Keyword / Symbol |
|---|---|
| `TokMUL`, `TokAndLogical`, `TokLESS`/`TokGREATER` | `*` (deref), `&` (addr-of), `<`/`>` (generics) — reused operator tokens |
| `TokAlloc`, `TokFree`, `TokLen`, `TokMUT` | `alloc`, `free`, `len`, `mut` |
| `TokReuse`, `TokExport`, `TokImport`, `TokAs` | `reuse`, `export`, `import`, `as` |
| `TokTypeclass`, `TokInstance` | `typeclass`, `instance` |
| `TokType`, `TokCase`, `TokFatArrow` | `type`, `case`, `=>` |
| `TokIf`, `TokElse`, `TokWhile` | `if`, `else`, `while` |
| `TokPipe`, `TokEllipsis` | `\|>`, `...` |
| `TokNum` | Number literals with optional suffix (`42i32`, `3.14f64`) — suffix consumed as part of lexeme |
| `TokStr`, `TokChar` | Double-quoted / single-quoted literals with escape sequences |

Removed: `TokSizeOf` — `sizeof` is now a typeclass method call.

### New Operator Token Checklist
1. Add enum value to `TokenType` in `include/lex/Token.h`
2. Add `{TokFoo, "foo"}` to `TokenMap` in `include/lex/Token.h`
3. If comparison: add to `Token::is_comparison()`
4. Add lexer handler in `src/lex/Lexer.cpp` — multi-char ops need lookahead
5. Add `{TokenType::TokFoo, precedence}` to `binopPrecedence` in `src/Parser.cpp`
6. Add codegen case to `TypeConverter::get_cmp_func()` or binary op handler

## Parser

- Committed/non-committed error model: `SUCCESS`, `FAILED`, `NONCOMMITTED`
- `p<T>` = `ParseResult<T>` — contains `node` (`unique_ptr<T>`), `status` (`ParserError`), helpers `.ok()`, `.failed()`, `.uncommitted()`
- `tryParsers<T>(Fns...)` — variadic template, returns first non-`NONCOMMITTED` result

### `ParsePrimaryExpr()` Order
```
ParseUnaryNegExpr, ParseDerefExpr, ParseAddrOfExpr, ParseAllocExpr,
ParseFreeExpr, ParseLenExpr, ParseArrayLiteralExpr, ParseCallExpr,
ParseParenExpr, ParseIfExpr, ParseCaseExpr, ParseWhileExpr,
ParseNumberExpr, ParseBoolExpr, ParseCharExpr, ParseStringExpr
```
Unary/keyword parsers go before `ParseCallExpr` (start with operator/keyword tokens, not `TokID`). After primary parse → `parsePostfixOps()` for `[index]` and `.field`.

### Key Parse Rules

| Syntax | Parser / AST | Notes |
|---|---|---|
| `alloc<T>(count)` | `ParseAllocExpr` → `AllocExprAST` | Type arg in angle brackets, then count expr |
| `free(expr)`, `len(expr)` | `ParseFreeExpr`/`ParseLenExpr` | Simple `keyword(expr)` pattern |
| `x \|> f` / `x \|> f(y,z)` | Desugared in `ParseBinaryExpr()` | → `f(x)` / `f(x,y,z)` at parse time. Precedence 1 (lowest). No special typecheck/codegen. |
| `while cond { body }` | `ParseWhileExpr` → `WhileExprAST` | Unit-typed expression |
| `import mod;` / `import mod as alias;` | `ParseImport` → `ImportDecl` | `alias_to_module` map resolves `alias::member` → `module__member` |
| `export let`/`struct`/`type`/`reuse` | Prefix flag | `is_exported` / `is_exposed` on respective AST nodes |
| `typeclass Name<T> { sig; ... }` | `ParseTypeClassDecl` → `TypeClassDeclAST` | Method signatures only (no bodies) |
| `instance Name<Type> { let m() { } }` | `ParseTypeClassInstance` → `TypeClassInstanceAST` | Full `FuncDefAST` method implementations |

Top-level `ParseDefinition()` tries: `ParseTypeClassDecl`, `ParseTypeClassInstance`, `ParseFuncDef`, `ParseStructDef`, `ParseEnumDef`, etc.

### Generic Type Parameters & Arguments
- **Definitions** `let f<T, U>(...)`: `ParsePrototype()` parses optional `<T, U, ...>` between name and params → stored in `proto->type_params`
- **Call sites** `f<i32, f64>(args)`: `ParseCallExpr()` delegates to `parseExplicitTypeArgsTail()` for speculative `<TypeExpr, ...>` parsing
- `consumeClosingAngleBracket()` splits `>>` → `>` + `>` and `>=` → `>` + `=` for nested generics

### `parseExplicitTypeArgsTail()` (Parser helper)

Extracted helper for speculative `<Type, ...>` parsing after a base name. Called by `ParseCallExpr()`. Handles three cases:
1. `Name<Types>::member(args)` — qualified after type args (enum variant or typeclass method): extends `qn` with member name and populates `explicit_type_args`
2. `Name<Types>(args)` — generic function call: populates `explicit_type_args`, `(` follows
3. Rollback — `<` was not a type arg list (e.g. comparison operator)

Uses `Lexer::save()`/`restore()` for rollback. Reuses `ParseTypeExpr()` and `consumeClosingAngleBracket()` for nested generics (e.g. `Option<ptr<i32>>`). Returns `vector<unique_ptr<TypeExprAST>>`.

### Enum Definitions, Type Aliases & Case Expressions
- `type Name = V1(Type) | V2;` — pipe-separated variants (`TokORLogical`). Generic: `type Option<T> = Some(T) | None;`
- `type IntAlias = i32;` — type alias (disambiguation: if after `=` next tokens are `ID |` or `ID(` → enum, otherwise → type alias)
- `case expr { Pattern => body, ... }` — expression returning a value
- Patterns: qualified `Color::Red`, unqualified `Red` (scope generator rewrites to qualified), wildcard `_`, payload `Some(v)`, generic `Option<i32>::Some(v)`
- See `type_checking.md` for enum type resolution details

### Struct Literals & Field Access
- `Name { field: value, ... }` → `StructLiteralExprAST` — lookahead in `ParseCallExpr` distinguishes from blocks
- `expr.field` → `FieldAccessExprAST` via `parsePostfixOps()`

### Arrays & Indexing
- `[expr, ...]` → `ArrayLiteralExprAST` | `expr[index]` → `IndexExprAST` via `parsePostfixOps()` | `[T;N]` → `ArrayTypeExprAST`

### Tuples & Destructuring
- `(expr, expr, ...)` → `TupleLiteralExprAST` — parsed in `ParseParenExpr()`: after first expr, if comma follows → parse more elements. Single expr with no comma → existing paren expr. `()` → `UnitExprAST`.
- `(T, U)` → `TupleTypeExprAST` — parsed in `ParseTypeExpr()`: after `(`, parse types. If `->` follows `)` → function type (existing). If no `->` with 2+ types → tuple type.
- `let (a, b) = expr;` → destructuring `VarDefAST` — parsed in `ParseVarDef()`: after `let [mut]`, if `(` → parse comma-separated `TypedVar`s → `is_tuple_destructure = true`, populate `destructure_vars`. Supports optional type annotations: `let (a: i32, b: bool) = t;`

## QualifiedName (`include/util/QualifiedName.h`)

Stores `vector<string> parts_` internally — supports arbitrary-depth names (`A::B::C::...`).

| Factory | Result |
|---|---|
| `::local("add")` | `["add"]` — unqualified local name |
| `::qualified("math", "add")` | `["math", "add"]` — module-qualified name |
| `::unresolved_qualified("x", "add")` | `["x", "add"]` + unresolved flag — alias not in `alias_to_module` |
| `::from_parts({"math","Color","Red"})` | `["math", "Color", "Red"]` — arbitrary depth |

| Accessor | Returns |
|---|---|
| `.get_name()` | Last part (`parts_.back()`) |
| `.get_module()` | First part if depth>1, else `""` |
| `.get_qualifier()` | All parts except last, joined with `::` |
| `.mangled()` | All parts joined with `::` — used for scope lookups, codegen, AND error messages |
| `.with_module("mod")` | Copy with module prepended (no-op if already qualified or mod is empty) |
| `.depth()` / `.parts()` | Number of segments / raw vector access |

**QualifiedName fields**: `CallExprAST::functionName`, `StructLiteralExprAST::struct_name`, `SimpleTypeExprAST::name`, `PrototypeAST::functionName`, `StructDefAST::struct_name`, `EnumDefAST::enum_name`
**Plain string**: `VariableExprAST::variableName`

### `parseQualifiedNameTail()` (Parser helper)

Unified helper in `Parser` that greedily consumes `::ID` pairs after an already-consumed first `TokID`. Parameters: `first_lexeme`, `first_loc`, `max_segments` (0=unlimited), `resolve_alias` (resolve first segment through `alias_to_module`). Returns `ParsedQualifiedName {qn, location}`. Used by `ParseStructDef`, `ParseEnumDef`, `ParsePrototype`, `ParseTypeExpr`, `ParseCallExpr`, and `ParseCaseExpr`. If `::` is not followed by `TokID`, rolls back and returns what it has.

## Type Expression Hierarchy

| Kind | Class | Syntax | Fields |
|---|---|---|---|
| `Simple` | `SimpleTypeExprAST` | `i32`, `math::Color` | `name` (QualifiedName) |
| `Pointer` | `PointerTypeExprAST` | `ptr<T>` | `pointee` |
| `Array` | `ArrayTypeExprAST` | `[T;N]` | `element`, `size` |
| `Function` | `FunctionTypeExprAST` | `(T, U) -> V` | `paramTypes`, `returnType` |
| `Tuple` | `TupleTypeExprAST` | `(T, U)` | `element_types` (vec of TypeExprAST) |
| `Generic` | `GenericTypeExprAST` | `Option<i32>` | `base_name` (QualifiedName), `type_args` |

Uses LLVM-style RTTI (`classof`, `llvm::dyn_cast`). NOT part of visitor pattern — resolved via `dynamic_cast` in type checker.

## AST Node Infrastructure
- `NodeKind` enum with LLVM-style RTTI in `include/ast/AstBase.h`
- `AST_NODE_METHODS(tree_name, kind_val)` macro → `getTreeName`, `classof`, `accept_vis`, `walk_with_preorder`, `walk_with_postorder`, `accept_synthesis`
- All nodes extend `AstBase` (via `ExprAST` or `DefinitionAST`) and implement `Visitable`
- ExprAST range: `FirstExpr` (VarDefAST) through `LastExpr` (WhileExprAST)
- DefinitionAST range: `FirstDef` (FuncDefAST) through `LastDef` (TypeClassInstanceAST)
- Non-range: `ProgramAST`, `PrototypeAST`, `TypedVarAST`, `BlockAST`

## AST Nodes — Quick Reference

### Definition Nodes
| Node | Key Fields |
|---|---|
| `FuncDefAST` | `Prototype`, `Block`, `is_exported` |
| `ExternAST` | `Prototype`, `is_exposed` |
| `StructDefAST` | `struct_name` (QN), `struct_members` (vec of TypedVarAST), `is_exported` |
| `EnumDefAST` | `enum_name` (QN), `variants` (vec of EnumVariantDef), `type_params`, `is_exported` |
| `TypeAliasDefAST` | `alias_name` (QN), `type_expr` (TypeExprAST), `resolved_type`, `is_exported` |
| `TypeClassDeclAST` | `class_name`, `type_param` (string), `methods` (vec of PrototypeAST) |
| `TypeClassInstanceAST` | `class_name`, `concrete_type_expr`, `concrete_type`, `methods` (vec of FuncDefAST) |

### Expression Nodes
| Node | Key Fields |
|---|---|
| `VarDefAST` | `is_mutable`, `is_tuple_destructure`, `TypedVar`, `destructure_vars`, `Expression` |
| `NumberExprAST` | `number` (string, e.g. "42i32") |
| `StringExprAST` | `string_content` |
| `BoolExprAST` | `b` (bool) |
| `CharExprAST` | `value` (char) |
| `BinaryExprAST` | `Op` (Token), `LHS`, `RHS`, `resolved_op_method` |
| `CallExprAST` | `functionName` (QN), `arguments`, `callee_func_type`, `is_partial`, `resolved_generic_name`, `type_bindings`, `explicit_type_args`, `is_typeclass_call`, `is_enum_constructor`, `enum_variant_index` |
| `ReturnExprAST` | `is_implicit`, `return_expr` |
| `UnitExprAST` | `is_implicit` — explicit `()` or implicit (empty block) |
| `VariableExprAST` | `variableName` (string), `is_enum_unit_variant`, `enum_variant_index` |
| `IfExprAST` | `bool_expr`, `thenBlockAST`, `elseBlockAST` |
| `DerefExprAST` | `operand` — prefix `*expr` |
| `AddrOfExprAST` | `operand` — prefix `&expr` |
| `AllocExprAST` | `type_arg` (TypeExprAST), `operand` (count) |
| `FreeExprAST` | `operand` |
| `ArrayLiteralExprAST` | `elements` (vec of ExprAST) |
| `IndexExprAST` | `array_expr`, `index_expr` |
| `LenExprAST` | `operand` |
| `UnaryNegExprAST` | `operand` — prefix `-expr` |
| `StructLiteralExprAST` | `struct_name` (QN), `field_names`, `field_values` |
| `FieldAccessExprAST` | `object_expr`, `field_name` |
| `CaseExprAST` | `scrutinee`, `arms` (vec of CaseArm) |
| `WhileExprAST` | `condition`, `body` |
| `TupleLiteralExprAST` | `elements` (vec of ExprAST) |

### Supporting Nodes
| Node | Key Fields |
|---|---|
| `ProgramAST` | `imports` (vec of ImportDecl), `DefinitionVec` |
| `PrototypeAST` | `functionName` (QN), `return_type_expr`, `parameterVectors`, `type_params`, `is_var_arg` |
| `TypedVarAST` | `name`, `is_mutable`, `type_expr` (nullable = no annotation) |
| `BlockAST` | `Statements` (vec of ExprAST) |
| `ImportDecl` | `module_name`, `alias`, `location` |

## New AST Node Checklist
1. Forward declare in `include/ast/AstDecl.h`
2. Add `NodeKind` entry in `include/ast/AstBase.h` (update `FirstExpr`/`LastExpr` or `FirstDef`/`LastDef` ranges)
3. Define class in `include/ast/Ast.h` (use `AST_NODE_METHODS` macro)
4. Add visitor methods to `AstBase.h` (`ASTVisitor` + `TypeCheckerVisitor`)
5. Add default `visit()` in `src/ast/Ast.cpp`
6. Add parser function in `Parser.h` and `Parser.cpp`
7. Wire into `ParsePrimaryExpr()` (expressions) or `ParseDefinition()` (definitions)
