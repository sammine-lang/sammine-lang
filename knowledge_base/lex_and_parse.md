# Lexer & Parser Patterns

## Lexer

Keywords recognized in `handleID()` (`src/lex/Lexer.cpp`). Token types defined in `include/lex/Token.h` (`TokenType` enum + `TokenMap`).

| Token(s) | Keyword / Symbol |
|---|---|
| `TokMUL`, `TokAndLogical`, `TokLESS`/`TokGREATER` | `*` (deref), `&` (addr-of), `<`/`>` (generics) — reused operator tokens |
| `TokAlloc`, `TokFree`, `TokLen`, `TokDim`, `TokMUT` | `alloc`, `free`, `len`, `dim`, `mut` |
| `TokReuse`, `TokExport`, `TokImport`, `TokAs` | `reuse`, `export`, `import`, `as` |
| `TokTypeclass`, `TokInstance` | `typeclass`, `instance` |
| `TokType`, `TokCase`, `TokFatArrow` | `type`, `case`, `=>` |
| `TokIf`, `TokElse`, `TokWhile` | `if`, `else`, `while` |
| `TokPipe`, `TokEllipsis` | `\|>`, `...` |
| `TokTick` | `'` (linear pointer prefix, e.g. `'ptr<T>`) |
| `TokFloorDiv`, `TokCeilDiv` | `/_` (floor division), `/^` (ceiling division) |
| `TokKernel` | `kernel` (kernel function definition prefix) |
| `TokNum` | Number literals with optional suffix (`42i32`, `3.14f64`) — suffix consumed as part of lexeme |
| `TokStr`, `TokChar` | Double-quoted / single-quoted literals with escape sequences |

Removed: `TokSizeOf` — `sizeof` is now a typeclass method call.

### Token Stream Rollback & Split

`TokenStream` (`include/lex/Token.h`) supports speculative parsing via rollback and token splitting:

| Method | Description |
|---|---|
| `mark_rollback()` | Saves the current cursor position (and records split state) |
| `rollback()` | Restores cursor to the marked position; undoes any `split_current` calls made since the mark |
| `rollback(size_t n)` | Steps cursor back by `n` tokens (no split undo) |
| `split_current(first_type, first_lex, second_type, second_lex)` | Replaces the current token with two tokens (e.g. `>>` into `>` + `>`). Split is tracked so `rollback()` can restore the original token |

Used by `parseExplicitTypeArgsTail()` and `consumeClosingAngleBracket()` for speculative generic parsing — if `<...>` turns out not to be type args, the stream is rolled back cleanly.

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
ParseFreeExpr, ParseLenExpr, ParseDimExpr, ParseArrayLiteralExpr,
ParseIdentifierExpr, ParseParenExpr, ParseIfExpr, ParseCaseExpr,
ParseWhileExpr, ParseNumberExpr, ParseBoolExpr, ParseCharExpr,
ParseStringExpr
```
Unary/keyword parsers go before `ParseIdentifierExpr` (start with operator/keyword tokens, not `TokID`). After primary parse -> `parsePostfixOps()` for `[index]` and `.field`.

### Key Parse Rules

| Syntax | Parser / AST | Notes |
|---|---|---|
| `alloc<T>(count)` | `ParseAllocExpr` -> `AllocExprAST` | Type arg in angle brackets, then count expr |
| `free(expr)`, `len(expr)`, `dim(expr)` | `ParseFreeExpr`/`ParseLenExpr`/`ParseDimExpr` | Simple `keyword(expr)` pattern; `dim` gets dimensions of nested arrays |
| `x \|> f` / `x \|> f(y,z)` | Desugared in `ParseBinaryExpr()` | -> `f(x)` / `f(x,y,z)` at parse time. Precedence 1 (lowest). No special typecheck/codegen. |
| `while cond { body }` | `ParseWhileExpr` -> `WhileExprAST` | Unit-typed expression |
| `import mod;` / `import mod as alias;` | `ParseImport` -> `ImportDecl` | `alias_to_module` map resolves `alias::member` -> `module__member` |
| `export let`/`struct`/`type`/`reuse` | Prefix flag | `is_exported` / `is_exposed` on respective AST nodes |
| `typeclass Name<T> { sig; ... }` | `ParseTypeClassDecl` -> `TypeClassDeclAST` | Method signatures only (no bodies) |
| `instance Name<Type> { let m() { } }` | `ParseTypeClassInstance` -> `TypeClassInstanceAST` | Full `FuncDefAST` method implementations |

Top-level `ParseDefinition()` tries: `ParseTypeClassDecl`, `ParseTypeClassInstance`, `ParseKernelDef`, `ParseStructDef`, `ParseEnumDef`, `ParseReuseDef`, `ParseFuncDef`.

### Kernel Parsing

`kernel name(params) -> T { kernel_exprs }` is parsed by `ParseKernelDef()` -> `KernelDefAST`.

- Prototype is parsed by the shared `ParsePrototype()` (same as CPU functions)
- Generic kernel functions are not yet supported (error if `proto->is_generic()`)
- Body is a `KernelBlockAST` containing `KernelExprAST` nodes (separate hierarchy from CPU `ExprAST`)

Kernel expression types (inside kernel body):

| Syntax | AST Node | Notes |
|---|---|---|
| `map(arr, (x: T) -> U { body })` | `KernelMapExprAST` | `input_name` (string), `lambda_proto` (PrototypeAST), `lambda_body` (BlockAST) — reuses CPU AST for lambda |
| `reduce(arr, +, 0)` | `KernelReduceExprAST` | `input_name` (string), `op_tok` (Token: `+`/`-`/`*`/`/`), `identity` (ExprAST) |
| number literal | `KernelNumberExprAST` | `number` (string) |

`KernelExprAST` is a separate base class (not `ExprAST`). It provides stub visitor methods — kernel exprs are not visited by CPU visitors. `KernelBlockAST` is a plain class (not an AST node), holding `vector<unique_ptr<KernelExprAST>>`.

### Generic Type Parameters & Arguments
- **Definitions** `let f<T, U>(...)`: `ParsePrototype()` parses optional `<T, U, ...>` between name and params -> stored in `proto->type_params`
- **Call sites** `f<i32, f64>(args)`: `ParseIdentifierExpr()` delegates to `parseExplicitTypeArgsTail()` for speculative `<TypeExpr, ...>` parsing
- `consumeClosingAngleBracket()` splits `>>` -> `>` + `>` and `>=` -> `>` + `=` for nested generics

### `parseExplicitTypeArgsTail()` (Parser helper)

Extracted helper for speculative `<Type, ...>` parsing after a base name. Called by `ParseIdentifierExpr()`. Handles three cases:
1. `Name<Types>::member(args)` — qualified after type args (enum variant or typeclass method): extends `qn` with member name and populates `explicit_type_args`
2. `Name<Types>(args)` — generic function call: populates `explicit_type_args`, `(` follows
3. Rollback — `<` was not a type arg list (e.g. comparison operator)

Uses `TokenStream::mark_rollback()`/`rollback()` for rollback. Reuses `ParseTypeExpr()` and `consumeClosingAngleBracket()` for nested generics (e.g. `Option<ptr<i32>>`). Returns `vector<unique_ptr<TypeExprAST>>`.

### Enum Definitions, Type Aliases & Case Expressions
- `type Name = V1(Type) | V2;` — pipe-separated variants (`TokORLogical`). Generic: `type Option<T> = Some(T) | None;`
- `type IntAlias = i32;` — type alias (disambiguation: if after `=` next tokens are `ID |` or `ID(` -> enum, otherwise -> type alias)
- `case expr { Pattern => body, ... }` — expression returning a value
- Patterns: qualified `Color::Red`, unqualified `Red` (scope generator rewrites to qualified), wildcard `_`, payload `Some(v)`, generic `Option<i32>::Some(v)`
- See `type_checking.md` for enum type resolution details

### Struct Literals & Field Access
- `Name { field: value, ... }` -> `StructLiteralExprAST` — lookahead in `ParseIdentifierExpr` distinguishes from blocks
- `expr.field` -> `FieldAccessExprAST` via `parsePostfixOps()`

### Arrays & Indexing
- `[expr, ...]` -> `ArrayLiteralExprAST` | `expr[index]` -> `IndexExprAST` via `parsePostfixOps()` | `[T;N]` -> `ArrayTypeExprAST`
- `[start...end]` -> `RangeExprAST` — detected inside `ParseArrayLiteralExpr` when `TokEllipsis` follows the first element

### Tuples & Destructuring
- `(expr, expr, ...)` -> `TupleLiteralExprAST` — parsed in `ParseParenExpr()`: after first expr, if comma follows -> parse more elements. Single expr with no comma -> existing paren expr. `()` -> `UnitExprAST`.
- `(T, U)` -> `TupleTypeExprAST` — parsed in `ParseTypeExpr()`: after `(`, parse types. If `->` follows `)` -> function type (existing). If no `->` with 2+ types -> tuple type.
- `let (a, b) = expr;` -> destructuring `VarDefAST` — parsed in `ParseVarDef()`: after `let [mut]`, if `(` -> parse comma-separated `TypedVar`s -> `is_tuple_destructure = true`, populate `destructure_vars`. Supports optional type annotations: `let (a: i32, b: bool) = t;`

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

Unified helper in `Parser` that greedily consumes `::ID` pairs after an already-consumed first `TokID`. Parameters: `shared_ptr<Token> first_tok`, `bool resolve_alias = true` (resolve first segment through `alias_to_module`). Returns `ParsedQualifiedName {qn, location}`. Used by `ParseStructDef`, `ParseEnumDef`, `ParsePrototype`, `ParseTypeExpr`, `ParseIdentifierExpr`, and `ParseCaseExpr`. If `::` is not followed by `TokID`, rolls back and returns what it has.

## Type Expression Hierarchy

| Kind | Class | Syntax | Fields |
|---|---|---|---|
| `Simple` | `SimpleTypeExprAST` | `i32`, `math::Color` | `name` (QualifiedName) |
| `Pointer` | `PointerTypeExprAST` | `ptr<T>` or `'ptr<T>` | `pointee`, `is_linear` |
| `Array` | `ArrayTypeExprAST` | `[T;N]` | `element`, `size` |
| `Function` | `FunctionTypeExprAST` | `(T, U) -> V` | `paramTypes`, `returnType` |
| `Tuple` | `TupleTypeExprAST` | `(T, U)` | `element_types` (vec of TypeExprAST) |
| `Generic` | `GenericTypeExprAST` | `Option<i32>` | `base_name` (QualifiedName), `type_args` |

Uses LLVM-style RTTI (`classof`, `llvm::dyn_cast`). NOT part of visitor pattern — resolved via `dynamic_cast` in type checker.

## AST Node Infrastructure
- `NodeKind` enum with LLVM-style RTTI in `include/ast/AstBase.h`
- `AST_NODE_METHODS(tree_name, kind_val)` macro -> `getTreeName`, `classof`, `accept_vis`, `walk_with_preorder`, `walk_with_postorder`, `accept_synthesis`
- All nodes extend `AstBase` (via `ExprAST` or `DefinitionAST`) and implement `Visitable`
- ExprAST range: `FirstExpr` (VarDefAST) through `LastExpr` (TupleLiteralExprAST)
- DefinitionAST range: `FirstDef` (FuncDefAST) through `LastDef` (KernelDefAST)
- KernelExprAST range: `FirstKernelExpr` (KernelNumberExprAST) through `LastKernelExpr` (KernelReduceExprAST) — separate from ExprAST
- Non-range: `ProgramAST`, `PrototypeAST`, `TypedVarAST`, `BlockAST`

### AST Fields vs ASTProperties

Semantic properties discovered during type checking live in the `ASTProperties` side table (`include/ast/ASTProperties.h`), **not** on AST nodes directly. Each node has a unique `NodeId`; properties are keyed by that ID.

| Props struct | Fields | Applies to |
|---|---|---|
| `CallProps` | `callee_func_type`, `is_partial`, `resolved_name`, `type_bindings`, `is_typeclass_call`, `is_enum_constructor`, `enum_variant_index` | `CallExprAST` |
| `VariableProps` | `is_enum_unit_variant`, `enum_variant_index` | `VariableExprAST` |
| `BinaryProps` | `resolved_op_method` | `BinaryExprAST` |
| `TypeAliasProps` | `resolved_type` | `TypeAliasDefAST` |
| `TypeClassInstanceProps` | `concrete_types` | `TypeClassInstanceAST` |

Node types (synthesized during type checking) are also stored in `ASTProperties` via `set_type()`/`get_type()` keyed by `NodeId`. `AstBase::get_type()` delegates to the current `ASTProperties` instance.

## AST Nodes — Quick Reference

### Definition Nodes
| Node | Key Fields |
|---|---|
| `FuncDefAST` | `Prototype`, `Block`, `is_exported` |
| `ExternAST` | `Prototype`, `is_exposed` |
| `StructDefAST` | `struct_name` (QN), `struct_members` (vec of TypedVarAST), `is_exported` |
| `EnumDefAST` | `enum_name` (QN), `variants` (vec of EnumVariantDef), `type_params`, `is_exported` |
| `TypeAliasDefAST` | `alias_name` (QN), `type_expr` (TypeExprAST), `is_exported` |
| `TypeClassDeclAST` | `class_name`, `type_params` (vec of string), `methods` (vec of PrototypeAST) |
| `TypeClassInstanceAST` | `class_name`, `concrete_type_exprs` (vec of TypeExprAST), `methods` (vec of FuncDefAST) |
| `KernelDefAST` | `Prototype` (PrototypeAST), `Body` (KernelBlockAST) |

### Expression Nodes
| Node | Key Fields |
|---|---|
| `VarDefAST` | `is_mutable`, `is_tuple_destructure`, `TypedVar`, `destructure_vars`, `Expression` |
| `NumberExprAST` | `number` (string, e.g. "42i32") |
| `StringExprAST` | `string_content` |
| `BoolExprAST` | `b` (bool) |
| `CharExprAST` | `value` (char) |
| `BinaryExprAST` | `Op` (Token), `LHS`, `RHS` |
| `CallExprAST` | `functionName` (QN), `arguments`, `explicit_type_args` |
| `ReturnStmtAST` | `is_implicit`, `return_expr` |
| `UnitExprAST` | `is_implicit` — explicit `()` or implicit (empty block) |
| `VariableExprAST` | `variableName` (string) |
| `IfExprAST` | `bool_expr`, `thenBlockAST`, `elseBlockAST` |
| `DerefExprAST` | `operand` — prefix `*expr` |
| `AddrOfExprAST` | `operand` — prefix `&expr` |
| `AllocExprAST` | `type_arg` (TypeExprAST), `operand` (count) |
| `FreeExprAST` | `operand` |
| `ArrayLiteralExprAST` | `elements` (vec of ExprAST) |
| `RangeExprAST` | `start`, `end` — `[start...end]` range syntax, parsed in `ParseArrayLiteralExpr` |
| `IndexExprAST` | `array_expr`, `index_expr` |
| `LenExprAST` | `operand` |
| `DimExprAST` | `operand` — `dim(expr)` gets dimensions of nested arrays |
| `UnaryNegExprAST` | `operand` — prefix `-expr` |
| `StructLiteralExprAST` | `struct_name` (QN), `field_names`, `field_values`, `explicit_type_args` |
| `FieldAccessExprAST` | `object_expr`, `field_name` |
| `CaseExprAST` | `scrutinee`, `arms` (vec of CaseArm) |
| `WhileExprAST` | `condition`, `body` |
| `TupleLiteralExprAST` | `elements` (vec of ExprAST) — `(expr, expr, ...)` parsed in `ParseParenExpr` |

### Kernel Expression Nodes
| Node | Key Fields |
|---|---|
| `KernelNumberExprAST` | `number` (string) |
| `KernelMapExprAST` | `input_name` (string), `lambda_proto` (PrototypeAST), `lambda_body` (BlockAST) |
| `KernelReduceExprAST` | `input_name` (string), `op_tok` (Token), `identity` (ExprAST) |

### Supporting Nodes
| Node | Key Fields |
|---|---|
| `ProgramAST` | `imports` (vec of ImportDecl), `DefinitionVec` |
| `PrototypeAST` | `functionName` (QN), `return_type_expr`, `parameterVectors`, `type_params`, `is_var_arg` |
| `TypedVarAST` | `name`, `is_mutable`, `type_expr` (nullable = no annotation) |
| `BlockAST` | `Statements` (vec of ExprAST) |
| `KernelBlockAST` | `expressions` (vec of KernelExprAST) — not an AstBase node |
| `ImportDecl` | `module_name`, `alias`, `location` |

## New AST Node Checklist
1. Forward declare in `include/ast/AstDecl.h`
2. Add `NodeKind` entry in `include/ast/AstBase.h` (update `FirstExpr`/`LastExpr` or `FirstDef`/`LastDef` ranges)
3. Define class in `include/ast/Ast.h` (use `AST_NODE_METHODS` macro)
4. Add visitor methods to `AstBase.h` (`ASTVisitor` + `TypeCheckerVisitor`)
5. Add default `visit()` in `src/ast/Ast.cpp`
6. Add parser function in `Parser.h` and `Parser.cpp`
7. Wire into `ParsePrimaryExpr()` (expressions) or `ParseDefinition()` (definitions)
