# Gotchas & Checklists

## Cross-Cutting Invariants
- **Error short-circuit**: any stage setting `this->error = true` skips all subsequent stages
- **Enum variant pre-qualification**: type checker assumes ALL variant calls arrive qualified — scope generator must rewrite unqualified variants before type checking
- **ASTProperties side table**: semantic properties (types, resolved names) live in `ASTProperties` keyed by `NodeId`, NOT on AST nodes — writers use `props_.call(id).field = val`, readers use `props_.call(id)->field`
- **Monomorphized defs at front**: injected at front of `DefinitionVec` so codegen processes them before call sites
- **Generic templates skipped**: BiTypeChecker, OpenTypeCheck, ViewPlacementCheck, NeverValueCheck, and codegen all skip `is_generic()` defs — only monomorphized copies are processed
- **`TypeKind::TypeParam` must never reach codegen** — `MIRTypes::mirType()` has no type for it, and declaring or lowering a function using it is an error
- **Names are decided only by `NameResolver`**: definitions get `module::name` (an executable's the module with no name: [, name], shown `name`), references are rewritten to those names, and a value's name left as one part is a local. A definition's symbol is its name (`QualifiedName::symbol`), but for a C binding's, its C name, and a program's `main` (`ASTProperties::symbol`). The parser keeps names as written; nothing after scope generation renames anything
- **Kernel lambdas**: a map lambda's body and a reduce's identity go through `MIRLowering::lowerKernelScalar`, which accepts only plain arith ops in one block, what a linalg region holds
- **`IdentityLayoutMap`**: kernel bufferization must use this so memref types match `kernel_abi::memrefOfPointer` output
- **Closures are stack-allocated**: cannot escape defining scope
- **`synthesize()` runs BEFORE `visit()`** for `CallExprAST` — detection logic must go in `synthesize`

## New TypeKind Checklist
1. Add to `TypeKind` enum (`Types.h`) — `-Wswitch` enforces all switch updates
2. Add `TypeData` variant class if compound
3. Add `Type::` factory method + `to_string()` case
4. Register in `enter_new_scope()` if named
5. Add a `smn::sizeOf()` case for its MIR type if it can be an enum payload
6. Add a `MIRTypes::mirType()` case, with a MIR type and its conversion to LLVM in `MIRToLLVM.cpp` if it needs one

## New AST Node Checklist
1. Forward declare in `AstDecl.h`
2. Add `NodeKind` in `AstBase.h` (update `FirstExpr`/`LastExpr` or `FirstDef`/`LastDef`)
3. Define class in `Ast.h` (`AST_NODE_METHODS` macro)
4. Add visitor methods to `AstBase.h` (`ASTVisitor` + `TypeCheckerVisitor`)
5. Add default `visit()` in `Ast.cpp`
6. Add parser in `Parser.h`/`Parser.cpp`, wire into `ParsePrimaryExpr()` or `ParseDefinition()`
7. Update 4 visitors: AstPrinter, BiTypeChecker, ScopeGenerator, GeneralSemantics
8. Add `lowerXxx()` in the `MIRLowering*.cpp` of its concern + dispatch in `lowerExpr()` (`MIRLowering.cpp`)
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
- **The ownership check follows parts**: a struct's fields, payloads, pointees are paths of their own; moving a whole moves its parts, and a branch join agrees each part
- **Checks after typing must skip generic templates**: template AST node IDs have no ASTProperties entries; only monomorphized copies have proper CallProps
- **FieldAccessExprAST in struct/array/tuple literals**: must handle `v.data` as consumption of child, not just `VariableExprAST`

## Parser Gotchas
- `if/else` as statements (not last expr) need trailing `;`
- `>>` split into `>` + `>` for nested generics via `split_current()`
- Struct literal vs block: lookahead in `ParseIdentifierExpr` checks `ID {` pattern
- `type` keyword: disambiguation — after `=`, if next tokens are `ID |` or `ID(` → enum, else → type alias
- Pipe `|>` desugared at parse time (precedence 1), no special typecheck/codegen

## Codegen Gotchas
- `if` lowers to `cf` blocks, not `scf.if`, so a `return` inside a branch is a plain `func.return` in its block
- A definition is linked as its name, every part (`QualifiedName::symbol`): `vec::push<i32>`, and a program's own `helper`, which is of the module with no name, as `::helper`, so no sammine function's symbol is a C function's (a program may define `printf` or `free`). Two symbols differ from the name, recorded by NameResolver in `ASTProperties::set_symbol`: a C binding is named after its module like any definition (`std::printf`, `io::printf`, `str::strlen`), as the same C function is bound in many modules, each with its own types, but its symbol is its C name (`@printf`); and a program's `main` is `main`. Codegen reads every symbol through `ASTProperties::symbol`, and declaring two definitions with one symbol is an error, not a merge. `ASTProperties::is_entry_point` says which function `main` is. A kernel's own function is `kernelSymbol` of its name (`__kernel__m__scale`): a GPU's assembler takes no `::`. Nothing reads a name back out of a symbol: a function op says what source calls it (`sammine.name`, read by `smn::sourceName` for messages) and what type it drops (`sammine.drops`, read by `smn::dropFunction`), both set by ModuleGen where it declares the function, and a kernel's wrapper is `llvm.no_inline`
- What a call's callee or a variable expression means is decided once, by the type checker, as a `Reference` (`CallProps::callee`, `VariableProps::value`): a local, a function by name, an enum's variant, or a built-in instance. MIR lowering reads it and never asks the module whether a function of that name exists. A local's name is one part and no definition's is, so a local `f` holding a function value shadows a function `f`, a method or a prelude name with no rule saying so
- Kernel wrappers get `noinline` to preserve LICM (LLVM can hoist calls when ptr args are loop-invariant)
- `GreenInterner` is NOT movable (pmr limitation) — always `make_unique`
- CST and AST diverge where parser desugars (pipe, `**` double deref, implicit returns)
- The dialect's generated headers (`SammineOps.h.inc`, …) are on the `SYSTEM` include path, as MLIR's own are: MLIR's generated code trips our `-Werror` warnings (C-style casts, narrowing), and it isn't ours to fix here. See `upstream/`
