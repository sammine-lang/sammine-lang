# Enum Support Plan

## Status

- **Phase 1** (DONE): Lexer + Parser + AST nodes
- **Phase 2** (DONE): Type system + Type checker
- **Phase 3** (DONE): LLVM + MLIR codegen for enum construction
- **Phase 4** (DONE): Match expression (case expressions)
- **Phase 5** (DONE): Generic enums + monomorphization
- **Phase 6** (DONE): Exhaustiveness checking

## Design Decisions

### Syntax
- **Definition**: `enum Color = Red | Green | Blue;` (OCaml-style pipe-separated)
- **Payloads**: `enum Shape = Circle(f64) | Rect(f64, f64) | Point;`
- **Construction**: Rust-style qualified — `Color::Red`, `Shape::Circle(3.14)`
- **Match** (Phase 4): `match expr { Pattern => body, }` — expression returning a value
- **Generics** (Phase 5): `enum Option<T> = Some(T) | None;`

### Variant Access
- **Qualified**: `Color::Red`, `Shape::Circle(3.14)` — always works
- **Unqualified**: `Red`, `Some(42)` — scope generator rewrites to qualified form before type checking
- Variant names ARE registered in the enclosing scope (for unqualified access)
- `ScopeGeneratorVisitor` populates `variant_to_enum` (variant name → enum name) during `preorder_walk(ProgramAST*)`
- `postorder_walk(CallExprAST*)` rewrites unqualified variant calls to qualified form using `variant_to_enum`
- The type checker assumes all enum variant calls arrive pre-qualified — it does NOT resolve unqualified names
- Both unit and payload variants parse as `CallExprAST` with a qualified name (after rewriting)
- Enums are NOT transitive through imports — file B must directly import file A

### LLVM Representation
- Tagged unions: `{ i32, [N x i8] }` where `i32` is the discriminant tag and `[N x i8]` is a byte buffer sized to the largest variant payload
- Named as `sammine.enum.<Name>` (e.g. `sammine.enum.Color`)
- Unit-only enums: just `{ i32 }` (no payload byte buffer)
- Construction: alloca + GEP + store for tag and payload fields
- Extraction (Phase 4): alloca + GEP + load from byte buffer at correct offsets

### Type System
- `TypeKind::Enum` with `EnumType` class
- `EnumType` uses `QualifiedName` for the name (not `std::string`)
- `EnumType` has NO `operator<` (only `operator==`, nominal by mangled name)
- `variant_constructors` map in BiTypeChecker: `variant_name → (enum_type, index)` — used for generic enum resolution (when `get_typename_type` fails) and `VariableExprAST` unit variant lookup
- Variant resolution in `synthesize(CallExprAST*)`: all calls arrive pre-qualified; look up enum type via `get_typename_type(module)`, fall back to `variant_constructors` for generic enums, then find variant by name

### Parser / Scope Resolution
- Parser's `resolveQualifiedName` returns `unresolved_qualified` when prefix isn't a module alias
- ScopeGeneratorVisitor:
  - `preorder_walk(ProgramAST*)`: registers variant names in scope and populates `variant_to_enum` map
  - `postorder_walk(CallExprAST*)`: rewrites unqualified variant calls to qualified form (e.g. `Red` → `Color::Red`), then returns early (skips scope check)
  - For already-qualified names: checks if prefix is an enum name in scope — if so, defers to type checker
- `variant_to_enum`: `std::map<std::string, std::string>` mapping variant name → enum name (declared in `ScopeGeneratorVisitor.h`)
- `visit(CallExprAST*)` has early return for `is_enum_constructor`

### Codegen (LLVM)
- `emitEnumConstructor` in `FunctionCodegen.cpp` — separate function
- `TypeConverter`: `register_enum_type` / `get_enum_type` (mirrors struct pattern)
- Forward declaration pass registers enum LLVM struct types before function codegen

### Codegen (MLIR)
- `enumTypes` map in `MLIRGenImpl` (parallel to `structTypes`)
- `emitEnumConstructor` in `MLIRGenExpr.cpp` using LLVM dialect: AllocaOp, GEPOp, StoreOp, LoadOp
- `getTypeSize` handles `TypeKind::Enum`: `4 + max_payload_size`
- `convertType` handles `TypeKind::Enum`: looks up in `enumTypes`

---

## Phase 4: Match Expression

**Goal**: Full `match` expression — parse, type-check, codegen.

### Token.h / Lexer.cpp
- Add `TokMatch` token and `{"match", TokMatch}` keyword
- Add `TokFatArrow` token: modify operator rule for `=` to add `{'>', TokFatArrow, "=>"}`

### AST (Ast.h, AstDecl.h)
- Add `MatchPattern` struct: `variant_name`, `bindings: vector<string>`, `is_wildcard: bool`, `location`
- Add `MatchExprAST : ExprAST` with `scrutinee`, `arms` (pattern + body)

### AstBase.h — visitor methods for MatchExprAST

### Parser.cpp
- `ParseMatchExpr()`: `match` → scrutinee expr → `{` → arms → `}`
- Each arm: pattern → `=>` → expr/block → `,`
- Pattern: `_` (wildcard), `Enum::Variant` (unit), `Enum::Variant(x, y)` (payload with bindings)
- Wire into `ParsePrimaryExpr()` on `TokMatch`

### BiTypeChecker
- `synthesize(MatchExprAST*)`:
  1. Synthesize scrutinee — must be `TypeKind::Enum`
  2. For each arm: look up variant in enum, validate binding count vs payload count
  3. Open scope per arm, register bindings as variables with payload types
  4. Synthesize arm body, collect result types
  5. All arms must return compatible types (use `Never` logic like if-else)
  6. Return common type

### CodegenVisitor
- `postorder_walk(MatchExprAST*)`:
  1. Evaluate scrutinee
  2. `extractvalue` tag at index 0
  3. Create `switch` on tag with basic blocks per arm
  4. Per arm: extract payload via alloca+GEP+load, create bindings, emit body, branch to merge
  5. Merge block with phi node for result
  6. Handle `Never` arms (no phi contribution)

### Monomorphizer.cpp
- Add `MatchExprAST` to `clone_expr()` — clone scrutinee + arms

### MLIRGen
- Mirror LLVM approach using LLVM dialect switch + basic blocks

### Key Design Notes
- Match arm scoping: each arm needs its OWN scope for pattern bindings (NOT like IfExprAST). Model after `visit(FuncDefAST*)` with `enter_new_scope()`/`exit_new_scope()`.
- Match codegen uses `switch i32 %tag`, NOT if-else chains.
- Arms that diverge (call `exit`) return `Never` — skip phi contribution.
- Pattern syntax uses qualified names: `Shape::Circle(r)`, `Color::Red`, `_`

---

## Phase 5: Generic Enums + Monomorphization

**Goal**: `enum Option<T> = Some(T) | None;` monomorphized to concrete types.

### Parser.cpp — ParseEnumDef
- After name, parse optional `<T, U, ...>` type params
- Store in `EnumDefAST::type_params`

### TypeExprAST — new GenericTypeExprAST
- Add `GenericTypeExprAST : TypeExprAST` with `name` + `type_args`
- Modify `ParseTypeExpr()`: if next token after ID is `<`, parse type args

### BiTypeChecker
- `visit(EnumDefAST*)`: if `type_params` non-empty, store in `generic_enum_defs`, skip concrete registration
- `resolve_type_expr()`: handle `GenericTypeExprAST` — instantiate via monomorphization

### Monomorphizer — instantiate_enum()
- Clone `EnumDefAST` with type substitution
- Insert monomorphized enum defs at front of `DefinitionVec`

---

## Phase 6: Exhaustiveness Checking

**Goal**: Error on non-exhaustive match, support `_` wildcard.

- After processing all arms, collect covered variants
- If no wildcard `_` present, check every variant is covered
- Emit error for missing variants
- Warn on duplicate variant patterns

---

## Key Risks

1. **-Wswitch cascade**: `TypeKind::Enum` triggers warnings in every switch — DONE (Phase 2)
2. **Variant name collisions**: Avoided by Rust-style qualified access
3. **Payload size/alignment**: Computed at codegen time via `DataLayout` (LLVM) / `getTypeSize` (MLIR)
4. **Multi-field payloads**: Need correct byte offsets within the payload buffer
5. **Monomorphizer clone_expr**: Must handle `MatchExprAST` before Phase 5
6. **Match as expression + Never type**: Arms that diverge skip phi contribution
7. **Match arm scoping**: Each arm needs its own scope
8. **Match codegen uses switch**: Not if-else — needs `switch i32 %tag` with N basic blocks + merge + phi
