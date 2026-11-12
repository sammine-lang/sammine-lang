# Gotchas & Checklists

## Cross-Cutting Invariants
- **Error short-circuit**: any stage setting `this->error = true` skips all subsequent stages
- **Enum variant pre-qualification**: type checker assumes ALL variant calls arrive qualified — scope generator must rewrite unqualified variants before type checking
- **ASTProperties side table**: semantic properties (types, resolved names) live in `ASTProperties` keyed by `NodeId`, NOT on AST nodes — writers use `props_.call(id).field = val`, readers use `props_.call(id)->field`
- **Monomorphized defs at front**: injected at front of `DefinitionVec` so codegen processes them before call sites
- **Generic templates skipped**: BiTypeChecker, LinearTypeChecker, and codegen all skip `is_generic()` defs — only monomorphized copies are processed
- **`TypeKind::TypeParam` must never reach codegen** — `convertType()` aborts on it
- **Names are decided only by `NameResolver`**: definitions get `module::name` (an executable's stay bare, a module's own C bindings keep their C name), references are rewritten to those names. The parser keeps names as written; nothing after scope generation renames anything
- **`in_kernel_lambda_body`**: must be true inside linalg body builders — prevents alloca/malloc/free emission
- **`IdentityLayoutMap`**: kernel bufferization must use this so memref types match `buildMemrefFromPtr` output
- **Closures are stack-allocated**: cannot escape defining scope
- **`synthesize()` runs BEFORE `visit()`** for `CallExprAST` — detection logic must go in `synthesize`

## New TypeKind Checklist
1. Add to `TypeKind` enum (`Types.h`) — `-Wswitch` enforces all switch updates
2. Add `TypeData` variant class if compound
3. Add `Type::` factory method + `to_string()` case
4. Register in `enter_new_scope()` if named
5. Add `convertType()` case in `MLIRGen.cpp`
6. Add `getTypeSize()` case if needed
7. Add `isFloatType()`/`isIntegerType()` if applicable

## New AST Node Checklist
1. Forward declare in `AstDecl.h`
2. Add `NodeKind` in `AstBase.h` (update `FirstExpr`/`LastExpr` or `FirstDef`/`LastDef`)
3. Define class in `Ast.h` (`AST_NODE_METHODS` macro)
4. Add visitor methods to `AstBase.h` (`ASTVisitor` + `TypeCheckerVisitor`)
5. Add default `visit()` in `Ast.cpp`
6. Add parser in `Parser.h`/`Parser.cpp`, wire into `ParsePrimaryExpr()` or `ParseDefinition()`
7. Update 4 visitors: AstPrinter, BiTypeChecker, ScopeGenerator, GeneralSemantics
8. Add `emitXxxExpr()` in `MLIRGenExpr.cpp` + dispatch in `emitExpr()`
9. If `ExprAST`: Monomorphizer must clone it — forgetting one causes `abort("Unknown ExprAST subclass")`

## New Operator Checklist
1. Token enum + `TokenMap` entry in `Token.h`
2. If comparison: add to `Token::is_comparison()`
3. Lexer handler in `Lexer.cpp` (multi-char needs lookahead)
4. Precedence entry in `binopPrecedence` (`Parser.cpp`)
5. Codegen handler (binary op chain or dedicated emitter)

## New Semantic Check Checklist
1. Decide pass: scope validation → ScopeGenerator, general → GeneralSemantics
2. Add visitor override in header
3. Implement in `.cpp`; use `add_error(location, message)`
4. Add E2E test in `e2e-tests/error_reporting/`

## Bug Patterns (from Vec<T> implementation)
- **Generic struct returns**: `resolve_type_expr` on `GenericTypeExprAST` with unresolved TypeParams must build placeholder StructType, not return Poisoned — `substitute()` alone can't trigger struct instantiation
- **Monomorphizer must preserve `is_linear`**: all type expression clones must carry linearity flag
- **Linear checker branch consistency must check children**: wrapper types (structs with linear fields) have consumption tracked in `VarInfo::children` at every depth, not just the parent — `reconcile` recurses
- **Linear checker must skip generic templates**: template AST node IDs have no ASTProperties entries; only monomorphized copies have proper CallProps
- **FieldAccessExprAST in struct/array/tuple literals**: must handle `v.data` as consumption of child, not just `VariableExprAST`

## Parser Gotchas
- `if/else` as statements (not last expr) need trailing `;`
- `>>` split into `>` + `>` for nested generics via `split_current()`
- Struct literal vs block: lookahead in `ParseIdentifierExpr` checks `ID {` pattern
- `type` keyword: disambiguation — after `=`, if next tokens are `ID |` or `ID(` → enum, else → type alias
- Pipe `|>` desugared at parse time (precedence 1), no special typecheck/codegen

## Codegen Gotchas
- `if` lowers to `cf` blocks, not `scf.if`, so a `return` inside a branch is a plain `func.return` in its block
- A C binding's symbol is its C name in every module (`std::printf` is `@printf`): `emitExtern` records it in `c_symbols`, and calls and function values look it up there
- Kernel wrappers get `noinline` to preserve LICM (LLVM can hoist calls when ptr args are loop-invariant)
- `GreenInterner` is NOT movable (pmr limitation) — always `make_unique`
- CST and AST diverge where parser desugars (pipe, `**` double deref, implicit returns)
- With `SAMMINE_SANITIZE`, our code has ASan but LLVM doesn't (`LLVM_USE_SANITIZER` empty). LLVM's allocators poison memory only in ASan-built code, so inline copies in our objects and in LLVM's disagree: a tool making an `MLIRContext` hits a false use-after-poison. `sammine-opt` sets `allow_user_poisoning=0` through `__asan_default_options`
- The dialect's generated headers (`SammineOps.h.inc`, …) are on the `SYSTEM` include path, as MLIR's own are: MLIR's generated code trips our `-Werror` warnings (C-style casts, narrowing), and it isn't ours to fix here. See `upstream/`
