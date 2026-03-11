# Semantic Analysis Passes

## Pipeline Position
```
lex → parse → resolve_imports → load_definitions → **ScopeGenerator → GeneralSemantics** → typecheck → codegen
```
Both passes run in `Compiler::semantics()` (`src/compiler/Compiler.cpp`). Each creates a fresh visitor, walks the full AST via `accept_vis()`, reports errors, and short-circuits on failure.

## ScopeGeneratorVisitor (`src/semantics/ScopeGeneratorVisitor.cpp`)

First semantic pass — builds lexical symbol table, validates all referenced names exist.

### Core Data
- **Scope stack**: `LexicalStack<Location>` — registers names + source locations
- **ASTContext** (via `ScopedASTVisitor::ctx()`) — tracks `in_imported_def`, `import_module`, `generic_type_params` for imported definition qualification. Pushed/popped automatically with `enter_new_scope()`/`exit_new_scope()`.
- **`variant_to_enum`**: `std::map<std::string, std::vector<QualifiedName>>` — maps variant name → owning enum names. Populated in `preorder_walk(ProgramAST*)` so unqualified variants can be rewritten to qualified form.

### Key Methods

**`preorder_walk(ProgramAST*)`** — Registers top-level defs (functions, externs, structs, enums). Detects duplicate names with `[SCOPE]` error pointing to both locations; import conflicts name the source module. For each enum variant not yet in scope, registers it and adds to `variant_to_enum`. Skips `TypeClassDeclAST`/`TypeClassInstanceAST`.

**`preorder_walk(VarDefAST*)`** — Registers local variable; errors on redefinition. For tuple destructuring (`is_tuple_destructure`), registers each var in `destructure_vars` instead. Allows shadowing in destructuring (needed for linear deref rebinding patterns).

**`preorder_walk(PrototypeAST*)`** — Registers function name in **parent** scope (via `register_name_parent()`/`can_see_parent()`) so it's visible at call-site level, not inside its own body. Registers each parameter in current scope; errors on duplicate params.

**`visit(ExternAST*)`** — Custom override: uses `enter_new_scope()`/`exit_new_scope()` (pushes ASTContext + scope stack), walks extern, visits prototype.

**`postorder_walk(CallExprAST*)`** — Skips calls with `explicit_type_args` (typeclass calls like `sizeof<i32>()` — resolved by type checker). Unresolved qualified names (e.g. `mod::func`): checks if module prefix (`get_module()`) is in scope, else emits "Module 'X' is not imported". **Unqualified enum variant rewrite**: looks up `functionName.get_name()` in `variant_to_enum`; if found, rewrites to qualified form (e.g. `Red(1)` → `Color::Red(1)`). Normal lookup via `can_see()` (recursive, includes current scope) — finds both top-level functions and function-typed params. Qualified names with known qualifier prefix (`get_qualifier()`) are allowed through (deferred to type checker). For 3-part names like `module::enum::variant`, `get_qualifier()` returns `"module::enum"` which is the registered enum name in scope.

**`postorder_walk(VariableExprAST*)`** — Validates name exists via `can_see()`. Also handles unqualified unit enum variants (appear as `VariableExprAST` — no arguments).

**`visit(CaseExprAST*)`** — Only visits scrutinee; arm bodies NOT walked — per-arm payload bindings created by type checker.

**`visit(TypeClassDeclAST*)`/`visit(TypeClassInstanceAST*)`** — No-ops.

### Scope Helpers
- `can_see(symbol)` — recursive through current + all parent scopes
- `can_see_parent(symbol)` — recursive from parent scope only
- `register_name(symbol, loc)` / `register_name_parent(symbol, loc)` — register in current/parent scope
- `enter_new_scope()` / `exit_new_scope()` — push/pop via `ScopedASTVisitor` infrastructure

## GeneralSemanticsVisitor (`src/semantics/GeneralSemanticsVisitor.cpp`)

Second semantic pass — enforces reserved identifier rules, inserts implicit returns.

### Core Data
- **`func_blocks`**: `std::map<BlockAST*, bool>` — maps function body blocks → whether they return unit. Populated via `ast->returnsUnit()`.
- **`returned`**: `bool` — tracks explicit `return` in current function body. Reset on non-function block entry and after processing each function block.
- **`scope_stack`**: `LexicalStack<Location>` — only used for scope push/pop infrastructure, not lookups.

### Reserved Identifier Checking

**`check_reserved_identifier(name, loc)`** — Rejects names containing `__` (reserved for C interop mangling); suggests single `_`.

Checked in `preorder_walk` for: `FuncDefAST` (function name), `VarDefAST` (variable name — handles destructuring vars individually), `StructDefAST` (struct name), `EnumDefAST` (enum name).

### Implicit Return Insertion

**`preorder_walk(FuncDefAST*)`** — Records block in `func_blocks` with unit-return flag.

**`preorder_walk(BlockAST*)`** — Non-function blocks: resets `returned = false`.

**`preorder_walk(ReturnStmtAST*)`** — Sets `returned = true`.

**`postorder_walk(BlockAST*)`** — Only acts on function blocks. If no explicit return found:
- **Unit-returning**: appends `return ()` (`ReturnStmtAST` wrapping `UnitExprAST`)
- **Value-returning**: wraps last statement in implicit `return`, only if block is non-empty, last statement isn't already `ReturnStmtAST`, and last statement is an expression (`is_statement == false`)

Resets `returned = false` after each function block.

### Typeclass Handling
- `visit(TypeClassDeclAST*)` — no-op (declarations have only prototypes)
- `visit(TypeClassInstanceAST*)` — visits each method's `FuncDefAST` so implicit return wrapping applies

## Adding a New Semantic Check Checklist
1. Decide which pass: scope validation → ScopeGenerator, general validation/AST rewrites → GeneralSemantics
2. Add visitor method override (`preorder_walk`/`postorder_walk`) in the header
3. Implement in `.cpp`; use `add_error(location, message)` from `Reportee` base class
4. Add E2E test in `e2e-tests/error_reporting/` with `// CHECK:` matching the error
