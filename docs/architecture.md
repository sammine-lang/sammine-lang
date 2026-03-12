# sammine-lang Developer Architecture Guide

This document describes the internal architecture of the sammine-lang compiler: a compiled language built with C++23 and LLVM/MLIR. It is intended for developers contributing to the compiler itself.

---

## 1. Compiler Pipeline

The compiler is orchestrated by `Compiler::start()` in `src/compiler/Compiler.cpp`. Each source file flows through a fixed sequence of stages, each of which can set an error flag that aborts subsequent stages.

**lex** -- The lexer (`src/lex/Lexer.cpp`) scans the input string into a `TokenStream` of `Token` objects. Each token carries its `TokenType`, lexeme string, and a byte-offset `Location`. The lexer is standalone and has no dependencies on later stages.

**parse** -- The recursive-descent parser (`src/Parser.cpp`) consumes the `TokenStream` and builds a `ProgramAST` -- a vector of `DefinitionAST` nodes plus `ImportDecl` entries. The parser uses `join_location()` to aggregate source spans onto AST nodes and sets the `pe` (parser error) flag on nodes it could not fully parse. After parsing, the compiler checks whether a `main` function exists; this determines whether the output is an executable or a library.

**resolve_imports** -- For each `import` declaration, the compiler searches CWD, `-I` paths, the source file's directory, and the stdlib directory for a `.mn` file. Found modules are parsed on the fly (no separate interface files). Exported functions become `ExternAST` nodes, while generic functions are copied wholesale so the monomorphizer can instantiate them. Transitive imports are resolved recursively; generic functions and exported type definitions (structs, enums, type aliases) propagate transitively, while non-generic functions, externs, and typeclass declarations/instances do not. Each direct import also searches for a pre-compiled `.so` or `.a` artifact to link against.

**load_definitions** -- If the program has a `main` function, the compiler loads `definitions.mn` from the stdlib directory and prepends its definitions to the program's `DefinitionVec`. This injects compiler-known type definitions and helper functions.

**semantics** -- Two visitor passes run in sequence. First, `ScopeGeneratorVisitor` (`src/semantics/ScopeGeneratorVisitor.cpp`) walks the AST to build a lexical symbol table, registering top-level names (functions, structs, enums, type aliases) and detecting duplicates. It also qualifies imported names with their module prefix and registers enum variant names for unqualified access. Second, `GeneralSemanticsVisitor` (`src/semantics/GeneralSemanticsVisitor.cpp`) performs semantic checks that do not require type information, such as validating that `return` appears only inside functions.

**typecheck** -- `BiTypeCheckerVisitor` (`src/typecheck/BiTypeChecker.cpp`, `BiTypeCheckerSynthesize.cpp`) performs bidirectional type checking in three internal passes over the `DefinitionVec`: (1) register all struct, enum, type alias, and typeclass definitions; (2) pre-register all function signatures so mutual recursion works; (3) fully type-check every definition body. When a generic function or type is called with concrete type arguments, the monomorphizer (`src/typecheck/Monomorphizer.cpp`) clones the AST, substitutes type parameters, and re-type-checks the clone. Monomorphized definitions are collected and later injected at the front of `DefinitionVec`.

**linear_check** -- `LinearTypeChecker` (`src/typecheck/LinearTypeChecker.cpp`) enforces that heap-allocated (`'ptr<T>`) pointers are consumed exactly once before scope exit via `free()`, `let` move, or `return`. It tracks variable state (Unconsumed/Consumed) per scope and verifies branch consistency (all if/case arms must consume or all must not consume each linear variable). It also forbids consuming outer linear variables inside loops.

**dump_ast** -- If `--ast-ir` is passed, the AST is pretty-printed via `ASTPrinter::print()`. If any prior stage set the error flag, the compiler aborts here with exit code 1.

**codegen** -- `MLIRGenImpl` (`src/codegen/MLIRGen.cpp`, `MLIRGenExpr.cpp`, `MLIRGenFunction.cpp`) translates the typed AST into an MLIR module using the Arith, Func, LLVM, SCF, and CF dialects. This stage uses direct recursive dispatch (not the visitor pattern). See Section 6 for details.

**optimize** -- The LLVM IR module (produced by MLIR lowering) is run through LLVM's `O2` optimization pipeline using the new pass manager. The `--llvm-ir` flag can dump pre-optimization, post-optimization, or diff output.

**emit_object** -- The optimized LLVM IR is lowered to a native `.o` object file using the target machine's code emission passes.

**emit_library** -- If the source file has no `main` function, the `.o` file is packaged into a shared library (`.so`, default) or static archive (`.a`), depending on the `--lib` flag. Runtime object files (`<stem>_runtime.o`) are auto-detected and included.

**link** -- If the source has a `main`, the compiler invokes `clang++` (falling back to `g++`) to link the `.o` file plus any imported library artifacts into a final executable.

---

## 2. Source Layout

Headers in `include/` mirror the directory structure of implementations in `src/`. All code lives under the `sammine_lang` namespace (utilities under `sammine_util`).

### `src/` and `include/`

| Directory | Key Files | Purpose |
|-----------|-----------|---------|
| `src/lex/` | `Lexer.cpp` | Lexer implementation. Produces `TokenStream`. |
| `include/lex/` | `Lexer.h`, `Token.h` | Token types, `TokenType` enum, `Token` class. |
| `src/` | `Parser.cpp` | Recursive-descent parser. |
| `include/parser/` | `Parser.h` | Parser class, precedence table, `alias_to_module` map for imports. |
| `src/ast/` | `Ast.cpp`, `AstToString.cpp`, `AstPrinterVisitor.cpp` | AST node method bodies, `to_string()` implementations, tree printer. |
| `include/ast/` | `Ast.h`, `AstBase.h`, `AstDecl.h`, `ASTProperties.h` | All AST node classes, visitor base classes, externalized properties. |
| `src/semantics/` | `ScopeGeneratorVisitor.cpp`, `GeneralSemanticsVisitor.cpp` | Scope/name resolution, general semantic checks. |
| `include/semantics/` | `ScopeGeneratorVisitor.h`, `GeneralSemanticsVisitor.h` | Visitor declarations. |
| `src/typecheck/` | `BiTypeChecker.cpp`, `BiTypeCheckerSynthesize.cpp`, `LinearTypeChecker.cpp`, `Monomorphizer.cpp`, `Types.cpp` | Type checking, synthesis, linear checking, generic instantiation, type utilities. |
| `include/typecheck/` | `BiTypeChecker.h`, `LinearTypeChecker.h`, `Monomorphizer.h`, `Types.h` | Type system definitions, checker/monomorphizer declarations. |
| `src/codegen/` | `MLIRGen.cpp`, `MLIRGenExpr.cpp`, `MLIRGenFunction.cpp`, `MLIRLowering.cpp`, `SammineJIT.cpp` | MLIR generation (split across three files), MLIR-to-LLVM-IR lowering, JIT infrastructure. |
| `include/codegen/` | `MLIRGen.h`, `MLIRGenImpl.h`, `MLIRLowering.h`, `LLVMRes.h`, `SammineJIT.h` | Codegen class, LLVM resource wrapper, JIT declarations. |
| `src/compiler/` | `Compiler.cpp` | Pipeline orchestration (the `Compiler` class is defined and implemented entirely in this file). |
| `include/compiler/` | `Compiler.h` | `CompilerRunner`, `compiler_option_enum`, and `LibFormat`. |
| `src/util/` | `Utilities.cpp`, `Logging.cpp` | File I/O, `abort()`, reporter rendering, debug logging. |
| `include/util/` | `Utilities.h`, `LexicalContext.h`, `QualifiedName.h`, `Logging.h`, `FileRAII.h` | Location, Reportee, Reporter, scoped symbol tables, qualified names with per-part type args. |
| `src/` | `sammine.cpp` | `main()` entry point: CLI argument parsing, invokes `CompilerRunner::run()`. |

### Test and Support Directories

| Directory | Purpose |
|-----------|---------|
| `e2e-tests/` | End-to-end tests using LLVM `lit` + `FileCheck`. Subdirectories under `compilables/` organized by feature: `arith/`, `array/`, `control/`, `enums/`, `func/`, `functions/`, `generics/`, `import/`, `io/`, `linear/`, `misc/`, `ptr/`, `type_inference/`, `typeclass/`, `types/`, `vec/`, etc. Error reporting tests in `error_reporting/`. Project Euler solutions in `euler/`. |
| `unit-tests/` | GoogleTest-based unit tests for the lexer and parser. |
| `stdlib/` | Standard library: `definitions.mn` (auto-loaded builtins), `std.mn`, `io.mn`, `io_runtime.c`, `vec.mn`, `str.mn`. |
| `knowledge_base/` | Developer documentation for each compiler subsystem. |

---

## 3. AST Design

### NodeKind and LLVM-style RTTI

Every AST node carries a `NodeKind` value assigned at construction time. The enum is partitioned into ranges:

```cpp
enum class NodeKind {
  // Standalone nodes
  ProgramAST, PrototypeAST, TypedVarAST, BlockAST,

  // ExprAST subclasses [FirstExpr..LastExpr]
  FirstExpr,
  VarDefAST = FirstExpr,
  NumberExprAST, StringExprAST, BoolExprAST, CharExprAST,
  BinaryExprAST, CallExprAST, ReturnStmtAST, UnitExprAST,
  VariableExprAST, IfExprAST, DerefExprAST, AddrOfExprAST,
  AllocExprAST, FreeExprAST, ArrayLiteralExprAST, IndexExprAST,
  LenExprAST, UnaryNegExprAST, StructLiteralExprAST,
  FieldAccessExprAST, CaseExprAST, WhileExprAST,
  TupleLiteralExprAST,
  LastExpr = TupleLiteralExprAST,

  // DefinitionAST subclasses [FirstDef..LastDef]
  FirstDef,
  FuncDefAST = FirstDef,
  ExternAST, StructDefAST, EnumDefAST, TypeAliasDefAST,
  TypeClassDeclAST, TypeClassInstanceAST,
  LastDef = TypeClassInstanceAST,
};
```

The `FirstExpr..LastExpr` and `FirstDef..LastDef` ranges enable LLVM-style RTTI via `classof()`. `ExprAST::classof` checks `getKind() >= FirstExpr && getKind() <= LastExpr`, and `DefinitionAST::classof` does the same for `FirstDef..LastDef`. This allows `llvm::dyn_cast<ExprAST>(node)` and `llvm::isa<DefinitionAST>(node)` throughout the codebase, avoiding C++ `dynamic_cast`.

### AstBase

`AstBase` (in `include/ast/AstBase.h`) is the root of the AST hierarchy. It provides:

- **Location tracking** via `join_location()`. During parsing, each node incrementally builds its source span by joining the locations of its children and tokens. The `|=` operator on `Location` takes the min of `source_start` and max of `source_end`.
- **Parser error flag** `pe`. Set to `true` when a child is `nullptr` (indicating a parse error), allowing downstream passes to skip malformed subtrees.
- **Node identity** via `NodeId` (a `uint32_t` from an atomic counter). Each node gets a unique ID at construction.
- **Type storage** via `ASTProperties`. Types are stored externally in an `ASTProperties` map keyed by `NodeId`, accessed through `get_type()` / `set_type()`. A static `current_props_` pointer is set once before type checking begins.

### ExprAST vs DefinitionAST

The AST has two major sub-hierarchies below `AstBase`:

- **`ExprAST`** -- expressions that produce values. Includes literals (`NumberExprAST`, `StringExprAST`, `BoolExprAST`, `CharExprAST`), operations (`BinaryExprAST`, `UnaryNegExprAST`), variable access (`VariableExprAST`, `FieldAccessExprAST`, `IndexExprAST`, `DerefExprAST`), control flow (`IfExprAST`, `CaseExprAST`, `WhileExprAST`), constructors (`StructLiteralExprAST`, `ArrayLiteralExprAST`, `TupleLiteralExprAST`), calls (`CallExprAST`), and others. `VarDefAST` is also an expression (it appears in blocks alongside other statements).
- **`DefinitionAST`** -- top-level definitions. Includes `FuncDefAST`, `ExternAST`, `StructDefAST`, `EnumDefAST`, `TypeAliasDefAST`, `TypeClassDeclAST`, and `TypeClassInstanceAST`.

Some nodes like `ProgramAST`, `PrototypeAST`, `TypedVarAST`, and `BlockAST` inherit directly from `AstBase` without going through either sub-hierarchy.

### TypeExprAST Hierarchy

Type annotations in source code are represented by a separate `TypeExprAST` hierarchy that is NOT part of the visitor pattern. Instead, type expressions are resolved via `llvm::dyn_cast` (using LLVM-style RTTI with a `ParseKind` discriminator) in the type checker's `resolve_type_expr()` method. The subclasses are:

- `SimpleTypeExprAST` -- named types like `i32`, `MyStruct`
- `PointerTypeExprAST` -- `ptr<T>` or `'ptr<T>` (linear)
- `ArrayTypeExprAST` -- `[T; N]`
- `FunctionTypeExprAST` -- `(T, U) -> V`
- `GenericTypeExprAST` -- `Option<i32>`, `Vec<T>`
- `TupleTypeExprAST` -- `(T, U)`

### AST_NODE_METHODS Macro

Each concrete AST node class uses the `AST_NODE_METHODS` macro, which generates six boilerplate methods:

```cpp
#define AST_NODE_METHODS(tree_name, kind_val)
  std::string getTreeName() const override;      // returns tree_name string
  static bool classof(const AstBase *node);       // LLVM RTTI: checks kind_val
  void accept_vis(ASTVisitor *visitor) override;  // calls visitor->visit(this)
  void walk_with_preorder(ASTVisitor *visitor) override;   // calls visitor->preorder_walk(this)
  void walk_with_postorder(ASTVisitor *visitor) override;  // calls visitor->postorder_walk(this)
  Type accept_synthesis(TypeCheckerVisitor *visitor) override; // calls visitor->synthesize(this)
```

### ASTProperties

Type information and per-node metadata are stored externally in `ASTProperties` (`include/ast/ASTProperties.h`), keyed by `NodeId`. This keeps AST nodes lightweight and allows type data to be set/queried from any pass without mutating the AST structure. Property bags include:

- `CallProps` -- callee function type, partial application flag, resolved monomorphized name, type bindings, enum constructor flag, typeclass call flag, enum variant index
- `VariableProps` -- enum unit variant flag and variant index
- `BinaryProps` -- resolved typeclass operator method name
- `TypeAliasProps` -- the resolved underlying type
- `TypeClassInstanceProps` -- concrete type arguments
- Node types -- the `Type` assigned to each node during type checking

---

## 4. Type System

### TypeKind Enum

The type system is defined in `include/typecheck/Types.h`. The `TypeKind` enum lists all type kinds:

| Kind | Description |
|------|-------------|
| `I32_t`, `I64_t`, `U32_t`, `U64_t` | Signed and unsigned integer types |
| `F64_t`, `F32_t` | Floating-point types |
| `Unit`, `Bool`, `Char`, `String` | Primitive types |
| `Function`, `Pointer`, `Array`, `Struct`, `Enum`, `Tuple` | Compound types (carry `TypeData`) |
| `Integer` | Pseudo-type: polymorphic integer literal (defaults to `I32_t`) |
| `Flt` | Pseudo-type: polymorphic float literal (defaults to `F64_t`) |
| `TypeParam` | Unresolved generic type parameter (e.g., `T`) |
| `Generic` | A generic template that has not been monomorphized |
| `Poisoned` | Indicates a type error has occurred; propagates silently |
| `NonExistent` | Type has not been assigned yet |
| `Never` | The type of diverging expressions (e.g., bare `return`) |

### Type Struct

The `Type` struct has four fields:

```cpp
struct Type {
  TypeKind type_kind;
  TypeData type_data;
  Mutability mutability;  // Immutable or Mutable
  Linearity linearity;    // NonLinear or Linear
};
```

`TypeData` is a variant:
```cpp
using TypeData = std::variant<
    FunctionType, PointerType, ArrayType, StructType,
    EnumType, TupleType, std::string, std::monostate>;
```

- Scalar types (integers, floats, bool, char, unit) use `std::monostate`.
- `TypeParam` and `String` use `std::string` to carry the parameter name or string content.
- Compound types use their respective wrapper classes (`FunctionType`, `PointerType`, etc.), each of which stores inner types via `TypePtr` (`shared_ptr<Type>`) to handle recursive type structures.

### Static Factory Methods

Types are constructed through static factory methods rather than direct construction:

```cpp
Type::I32_t()              // scalar
Type::Pointer(pointee)     // compound
Type::Array(element, size) // compound
Type::Struct(name, field_names, field_types)
Type::Enum(name, variants, is_integer_backed, backing_type)
Type::Function(params, var_arg)
Type::Tuple(element_types)
Type::TypeParam("T")       // generic parameter
```

### TypeMapOrdering

`TypeMapOrdering` defines a type lattice for subtyping relationships. Its `populate()` method registers built-in edges (e.g., `Integer` is the parent of `I32_t`, `I64_t`, `U32_t`, `U64_t`; `Flt` is the parent of `F64_t`, `F32_t`). Four key methods provide compatibility checks:

- `compatible_to_from(to, from)` -- full check including structure, mutability, and linearity. Used for assignments, function arguments, and return values.
- `structurally_compatible(to, from)` -- checks structural type equivalence (polymorphic numerics, tuples, enums). Does not check qualifiers.
- `qualifier_compatible(to, from)` -- checks mutability and linearity qualifiers (e.g., linear pointer types must match exactly).
- `lowest_common_type(a, b)` -- finds the lowest common ancestor in the type lattice, used for unifying branch types.

---

## 5. Visitor Pattern

The visitor infrastructure is defined in `include/ast/AstBase.h`.

### ASTVisitor

`ASTVisitor` is the base visitor class. For each AST node type, it declares three virtual methods:

- `visit(NodeType*)` -- the main entry point for visiting a node. The default implementation calls `preorder_walk`, then visits children, then calls `postorder_walk`.
- `preorder_walk(NodeType*)` -- called before children are visited. Pure virtual.
- `postorder_walk(NodeType*)` -- called after children are visited. Pure virtual.

`ASTVisitor` extends `Reportee`, so every visitor can accumulate errors and warnings that the `Reporter` renders after the pass completes.

### ScopedASTVisitor

`ScopedASTVisitor` extends `ASTVisitor` with `enter_new_scope()` and `exit_new_scope()` hooks. It overrides `visit(FuncDefAST*)` to push/pop a scope around function bodies. Both `ScopeGeneratorVisitor` and `BiTypeCheckerVisitor` extend this class.

### TypeCheckerVisitor

`TypeCheckerVisitor` is a separate interface (not derived from `ASTVisitor`) that declares a `synthesize(NodeType*)` method for each AST node type. Each `synthesize` method returns a `Type`. The `BiTypeCheckerVisitor` implements both `ScopedASTVisitor` and `TypeCheckerVisitor`.

Nodes invoke synthesis through `accept_synthesis(TypeCheckerVisitor*)`, which calls `visitor->synthesize(this)` (dispatched via the `AST_NODE_METHODS` macro).

### How the Passes Use Visitors

- **ScopeGeneratorVisitor**: uses `preorder_walk` to register names into the lexical scope stack before children are visited. Scope push/pop around function bodies is handled by the inherited `ScopedASTVisitor::visit(FuncDefAST*)`, not by `postorder_walk`.
- **GeneralSemanticsVisitor**: uses `preorder_walk` for pre-checks and `postorder_walk` for post-checks.
- **BiTypeCheckerVisitor**: overrides `visit()` for explicit traversal control (e.g., multi-pass registration in `visit(ProgramAST*)`). Uses `accept_synthesis` for the bidirectional type-checking core. The split between `visit()` (in `BiTypeChecker.cpp`) and `synthesize()` (in `BiTypeCheckerSynthesize.cpp`) keeps the two concerns in separate files.

---

## 6. MLIR Codegen

The MLIR code generator is implemented in `MLIRGenImpl` (declared in `include/codegen/MLIRGenImpl.h`, with method bodies split across `MLIRGen.cpp`, `MLIRGenExpr.cpp`, and `MLIRGenFunction.cpp`).

### Dispatch Model

Unlike the earlier passes, MLIR codegen does NOT use the visitor pattern. Instead, it uses direct recursive dispatch. The central `emitExpr(ExprAST*)` method contains a chain of `llvm::dyn_cast` calls:

```cpp
mlir::Value emitExpr(ExprAST *ast) {
  if (auto *num = llvm::dyn_cast<NumberExprAST>(ast))
    return emitNumberExpr(num);
  if (auto *bin = llvm::dyn_cast<BinaryExprAST>(ast))
    return emitBinaryExpr(bin);
  // ... one branch per ExprAST subclass
}
```

Top-level definitions are dispatched similarly in `emitDefinition(DefinitionAST*)`.

### MLIR Dialects Used

- **Arith** -- arithmetic operations on integers and floats (`arith.addi`, `arith.mulf`, `arith.cmpi`, etc.)
- **Func** -- function definitions and calls for the `exit` runtime function (`func.func`, `func.call`, `func.return`)
- **LLVM** -- low-level operations: `llvm.alloca`, `llvm.store`, `llvm.load`, `llvm.gep`, `llvm.func` (for externs, varargs, closures), `llvm.call`, struct types, pointer types, `llvm.mlir.undef`, `llvm.insertvalue`, `llvm.extractvalue`, `llvm.mlir.addressof`
- **SCF** -- structured control flow for bounds checks (`scf.if` with `scf.yield`)
- **CF** -- unstructured control flow for if/else and case expressions (`cf.cond_br`, `cf.br`)

### Generation Phases

`MLIRGenImpl::generate()` runs several phases:

1. **Runtime function declarations.** Declares `malloc`, `free`, and `exit` as LLVM/Func ops so they are available to `alloc`/`free` emission and bounds-check aborts. Also registers the closure struct type (`sammine.closure`).

2. **Pre-pass 1: Type registration.** Iterates over all `StructDefAST` and `EnumDefAST` nodes to create LLVM struct types (`LLVMStructType::getIdentified`) and store them in the `structTypes` / `enumTypes` maps. This must happen before any function signatures reference these types.

3. **Pre-pass 2: Forward-declare functions.** Iterates over all `FuncDefAST` and `TypeClassInstanceAST` nodes to emit `func.func` symbol declarations (without bodies). This enables mutual recursion and function-as-value references.

4. **Definition emission.** Iterates again, calling `emitDefinition()` for each node. Functions get full bodies; extern declarations are emitted via `emitExtern()`; struct/enum/type-alias/typeclass-decl nodes are no-ops (already handled). TypeClass instances emit each method as a regular function.

### Uniform Variable Model

All non-array local variables use a uniform `llvm.alloca` + `llvm.store` model. `emitVarDef` allocates stack space, stores the initial value, and registers the alloca pointer in the `symbolTable`. `emitVariableExpr` then loads the value with `llvm.load`. Array variables are the exception: the array literal emitter returns an `!llvm.ptr` directly, and variable references to arrays return the pointer without loading.

### Closures and Partial Application

Function-typed values are represented as closure fat pointers: `!llvm.struct<"sammine.closure", (ptr, ptr)>` containing a code pointer and an environment pointer. When a named function is used as a value, a wrapper function is generated that adapts the calling convention. Partial application creates a heap-allocated environment struct containing the bound arguments.

### Lowering Pipeline

`MLIRLowering.cpp` defines the pass pipeline that converts the multi-dialect MLIR module to pure LLVM IR:

1. `SCF -> ControlFlow` -- lowers structured control flow to branches
2. `Arith -> LLVM` -- lowers arithmetic ops to LLVM dialect ops
3. `ControlFlow -> LLVM` -- lowers cf.br/cf.cond_br to LLVM branches
4. `Func -> LLVM` -- lowers func.func/func.call/func.return to LLVM
5. `ReconcileUnrealizedCasts` -- cleans up remaining type casts

After lowering, `mlir::translateModuleToLLVMIR()` converts the MLIR LLVM dialect module into an `llvm::Module`.

---

## 7. Error Reporting

Error reporting uses three cooperating classes in `include/util/Utilities.h`:

### Location

`Location` stores a pair of byte offsets (`source_start`, `source_end`) into the original source string, plus an optional `SourceInfo` pointer for cross-file error locations (used by the import system). Locations compose via `operator|` (union of spans).

### Reportee

`Reportee` is the base class for any pass that can produce diagnostics. It stores a vector of `Report` tuples, each containing:
- A `Location` pointing to the source span
- A vector of message strings (multiple lines for multi-part errors)
- A `ReportKind` (error, warn, diag)
- A `std::source_location` capturing the C++ call site (shown in `--diagnostics=dev` mode)

Methods `add_error()`, `add_warn()`, and `add_diagnostics()` append reports. The `ASTVisitor` base class, the `Lexer`, and the `Parser` all extend `Reportee`.

### Reporter

`Reporter` is the rendering engine. After each pass, `Compiler::start()` calls `reporter.report(pass)` to render all accumulated reports. The renderer uses byte-offset-to-line mapping (`DiagnosticData`) to convert byte offsets to line/column positions, then prints ariadne-style diagnostic output with Unicode box-drawing characters, color-coded by severity, and source line context. In `--diagnostics=dev` mode, each error also shows the C++ file and line that emitted it.

`Reporter` also provides `immediate_error()`, `immediate_warn()`, and `immediate_diag()` for diagnostics that must be displayed immediately (e.g., during import resolution or codegen).

---

## 8. How to Add a New Feature

### Adding a New Type

1. Add a variant to `TypeKind` in `include/typecheck/Types.h`.
2. If the type carries data, add a new wrapper class (like `TupleType`) and add it to the `TypeData` variant.
3. Add a static factory method to `Type` (e.g., `Type::MyNewType()`).
4. Update `Type::to_string()` for the new kind.
5. Update every `switch` on `TypeKind`. The compiler will help you find them via `-Wswitch` -- expect hits in:
   - `Types.cpp` (`compatible_to_from`, `structurally_compatible`, `TypeMapOrdering::populate`)
   - `BiTypeCheckerSynthesize.cpp` (type resolution)
   - `MLIRGen.cpp` (`convertType`, `getTypeSize`, `isIntegerType`, `isFloatType`)
   - `MLIRGenExpr.cpp` (expression emission for the new type)
   - `MLIRGenFunction.cpp` (`buildFuncType`, `emitExtern`)
   - `LinearTypeChecker.cpp` (if the type can be linear)
6. Add parsing support in `Parser.cpp` if the type has new syntax.
7. Add e2e tests in `e2e-tests/compilables/types/`.

### Adding a New AST Node

1. Add a value to `NodeKind` in `include/ast/AstBase.h`. Place it in the correct range (`FirstExpr..LastExpr` for expressions, `FirstDef..LastDef` for definitions).
2. Add a forward declaration in `include/ast/AstDecl.h`.
3. Define the class in `include/ast/Ast.h` using `AST_NODE_METHODS("TreeName", NodeKind::MyNode)`.
4. Add `visit()`, `preorder_walk()`, and `postorder_walk()` declarations to `ASTVisitor` in `include/ast/AstBase.h`.
5. Add `synthesize()` declaration to `TypeCheckerVisitor` in `include/ast/AstBase.h`.
6. Implement default `visit()` in `src/ast/Ast.cpp` (typically: preorder, visit children, postorder).
7. Implement handling in every visitor:
   - `ScopeGeneratorVisitor` -- register/check names
   - `GeneralSemanticsVisitor` -- semantic validation
   - `BiTypeCheckerVisitor` -- `visit()` + `synthesize()` methods
8. Add to the `Monomorphizer` clone methods if the node can appear inside generic function bodies.
9. Add MLIR emission in `emitExpr()` / `emitDefinition()` dispatch chain.
10. Add `to_string()` in `src/ast/AstToString.cpp` and printer support in `AstPrinterVisitor.cpp`.
11. Add parser rules in `src/Parser.cpp`.

### Adding a New Expression

1. Follow the "Adding a New AST Node" checklist above with the node in the `FirstExpr..LastExpr` range.
2. Add parsing in `Parser.cpp`. If it is an infix operator, add it to the precedence table. If it is a prefix construct (like `alloc`, `free`, `len`), add a new parse method and call it from the appropriate entry point.
3. Implement `synthesize()` in `BiTypeCheckerSynthesize.cpp` to compute the expression's type.
4. Add an `emitMyExpr()` method to `MLIRGenImpl` and call it from the `emitExpr()` dispatch chain.

### Adding a New Definition

1. Follow the "Adding a New AST Node" checklist above with the node in the `FirstDef..LastDef` range.
2. Add parsing in `Parser.cpp`, called from the top-level definition loop.
3. In `ScopeGeneratorVisitor::preorder_walk(ProgramAST*)`, register the definition's name.
4. In `BiTypeCheckerVisitor::visit(ProgramAST*)`, add the definition to the appropriate registration pass (first pass for type defs, second for function signatures, third for full checking).
5. In `MLIRGenImpl::emitDefinition()`, add a `dyn_cast` branch for the new definition type.
6. In `Monomorphizer`, add cloning support if the definition can be generic.
7. Consider import system implications: update `resolve_imports()` in `Compiler.cpp` if the definition should be importable/exportable.
