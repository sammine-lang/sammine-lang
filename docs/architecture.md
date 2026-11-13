# sammine-lang Developer Architecture Guide

This document describes the internal architecture of the sammine-lang compiler: a compiled language built with C++23 and LLVM/MLIR. It is intended for developers contributing to the compiler itself.

---

## 1. Compiler Pipeline

The compiler is orchestrated by `Compiler::start()` in `src/compiler/Compiler.cpp`. Each source file flows through a fixed sequence of stages, each of which can set an error flag that aborts subsequent stages.

**lex** -- The lexer (`src/lex/Lexer.cpp`) scans the input string into a `TokenStream` of `Token` objects. Each token carries its `TokenType`, lexeme string, and a byte-offset `Location`. The lexer is standalone and has no dependencies on later stages.

**parse** -- The recursive-descent parser (`src/Parser.cpp`) consumes the `TokenStream` and builds a `ProgramAST` -- a vector of `DefinitionAST` nodes plus `ImportDecl` entries. The parser uses `join_location()` to aggregate source spans onto AST nodes and sets the `pe` (parser error) flag on nodes it could not fully parse. After parsing, the compiler checks whether a `main` function exists; this determines whether the output is an executable or a library.

**resolve_imports** -- For each `import` declaration, the compiler searches CWD, `-I` paths, the source file's directory, and the stdlib directory for a `.mn` file. Found modules are parsed on the fly (no separate interface files). Exported functions become `ExternAST` nodes, while generic functions are copied wholesale so the monomorphizer can instantiate them. A module's own imports are imported the same way, recursively, whichever module reaches them first, since its code compiled here (generic functions, instance copies) may use them; the program can only name modules it imports itself. Each module also searches for a pre-compiled `.so` or `.a` artifact to link against.

The prelude, `definitions.mn`, is imported this way by every module, library or executable, without an `import` line (except when compiling `definitions.mn` itself). Its names are used without a module prefix (`NameResolver::qualify` gives them none), and its exported instances are compiled into `definitions.so`.

**semantics** -- Two visitor passes run in sequence. First, `ScopeGeneratorVisitor` (`src/semantics/ScopeGeneratorVisitor.cpp`) walks the AST to build a lexical symbol table, registering top-level names (functions, structs, enums, type aliases) and detecting duplicates. It also qualifies imported names with their module prefix and registers enum variant names for unqualified access. Second, `GeneralSemanticsVisitor` (`src/semantics/GeneralSemanticsVisitor.cpp`) performs semantic checks that do not require type information, such as rejecting identifiers reserved for C interop. The AST isn't rewritten: a body that runs off its end returns its last expression (`FuncDefAST::body_value`), which the type checker, the linear checker and MIR lowering each read as such.

**typecheck** -- `BiTypeCheckerVisitor` (`src/typecheck/BiTypeChecker.cpp`, `BiTypeCheckerSynthesize.cpp`) performs bidirectional type checking in three internal passes over the `DefinitionVec`: (1) register all struct, enum, type alias, and typeclass definitions, so none has to be declared before it is used: every struct and enum is declared first and its fields or variants filled in after (`StructType`/`EnumType` share their body between copies, so a type embedded before it was filled in sees the fields), aliases are expanded when first needed, as in rustc, and a type or alias that refers to itself is an error; (2) pre-register all function signatures so mutual recursion works; (3) fully type-check every definition body. When a generic function or type is called with concrete type arguments, the monomorphizer (`src/typecheck/Monomorphizer.cpp`) clones the AST, substitutes type parameters, and re-type-checks the clone. Monomorphized definitions are collected and later injected at the front of `DefinitionVec`.

Once type checking has found nothing, two more passes of the stage check what only a finished typing shows: `OpenTypeCheck` (`src/typecheck/OpenTypeCheck.cpp`), a type argument nothing decided, and `ViewPlacementCheck` (`src/typecheck/ViewPlacementCheck.cpp`), a non-owning `ptr<T>` anywhere but a parameter's or local's whole type. What may be written through a view, have its address taken, or be bound by a closure is checked on MIR (**mir**).

**mir** -- Builds the program's MLIR (each function's body in MIR, see **codegen**) and runs the ownership check on it (`src/dialect/OwnershipCheck.cpp`, over the analysis in `OwnershipAnalysis.cpp`): every heap-allocated `'ptr<T>`, and every value holding one, is consumed exactly once on every path, through `drop()`, a move, or `return`, and none is still owned when its function returns. It is a dataflow analysis on MIR's control flow, so branches, loops and early returns need no special handling. Its errors are reported like any stage's.

**dump_ast** -- With `--emit ast`, the AST is pretty-printed via `ASTPrinter::print()` and the compiler stops. If any prior stage set the error flag, the compiler aborts here with exit code 1.

**codegen** -- `generateModules` (`src/codegen/ModuleGen.cpp`) builds the MLIR module: function declarations, C bindings, and kernels through `KernelGen` (`KernelGen.cpp`). Every function body goes through MIR, a mid-level IR in the `sammine` dialect (`MIRLowering.cpp`), which is lowered to the LLVM dialect first thing in the lowering to LLVM IR. See Section 6 for details.

**optimize** -- The LLVM IR module (produced by MLIR lowering) is run through LLVM's `O2` optimization pipeline using the new pass manager. `--emit llvm`, `--emit llvm-opt` and `--emit llvm-diff` print the IR before optimization, after it, or a diff of the two, then stop. `--emit mlir` prints the MLIR module before it is lowered to LLVM IR.

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
| `include/parser/` | `Parser.h` | Parser class, precedence table. Names are parsed as written; `NameResolver` resolves them. |
| `src/ast/` | `Ast.cpp`, `AstToString.cpp`, `AstPrinterVisitor.cpp` | AST node method bodies, `to_string()` implementations, tree printer. |
| `include/ast/` | `Ast.h`, `AstBase.h`, `AstDecl.h`, `ASTProperties.h` | All AST node classes, visitor base classes, externalized properties. |
| `src/semantics/` | `ScopeGeneratorVisitor.cpp`, `GeneralSemanticsVisitor.cpp` | Scope/name resolution, general semantic checks. |
| `include/semantics/` | `ScopeGeneratorVisitor.h`, `GeneralSemanticsVisitor.h` | Visitor declarations. |
| `src/typecheck/` | `BiTypeChecker.cpp`, `BiTypeCheckerSynthesize.cpp`, `OpenTypeCheck.cpp`, `ViewPlacementCheck.cpp`, `Monomorphizer.cpp`, `Types.cpp` | Type checking, synthesis, open type arguments, where views may be, generic instantiation, type utilities. |
| `include/typecheck/` | `BiTypeChecker.h`, `OpenTypeCheck.h`, `ViewPlacementCheck.h`, `Monomorphizer.h`, `Types.h` | Type system definitions, checker/monomorphizer declarations. |
| `src/codegen/` | `ModuleGen.cpp`, `KernelGen.cpp`, `MIRLowering*.cpp`, `MIRTypes.cpp`, `ScalarOps.cpp`, `MLIRLowering.cpp`, `SammineJIT.cpp` | The module (declarations, C bindings, linkage), kernels, function bodies to MIR (split by concern: calls, control flow, aggregates, places, operators), MLIR-to-LLVM-IR lowering, JIT infrastructure. |
| `include/codegen/` | `ModuleGen.h`, `KernelGen.h`, `MIRLowering.h`, `MIRTypes.h`, `MLIRLowering.h`, `LLVMRes.h`, `SammineJIT.h` | Codegen classes, LLVM resource wrapper, JIT declarations. |
| `src/dialect/`, `include/dialect/` | `SammineOps.td`, `SammineTypes.td`, `MIRToLLVM.cpp` | The `sammine` dialect, MIR's types and ops, and its lowering to the LLVM dialect. |
| `src/cabi/`, `include/cabi/` | `CABI.cpp`, `KernelABI.cpp` | Calling conventions: how C takes structs and variadic arguments, how a kernel's wrapper takes arrays. |
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
| `stdlib/` | Standard library: `definitions.mn` (the prelude, imported by every module), `std.mn`, `io.mn`, `io_runtime.c`, `vec.mn`, `str.mn`. |
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
  AllocExprAST, DropExprAST, ArrayLiteralExprAST, IndexExprAST,
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

- `compatible_to_from(to, from)` -- full check including structure and linearity. Used for assignments, function arguments, and return values.
- `structurally_compatible(to, from)` -- checks structural type equivalence (polymorphic numerics, tuples, enums). Does not check qualifiers.
- `qualifier_compatible(to, from)` -- checks linearity qualifiers (e.g., linear pointer types must match exactly).
- `lowest_common_type(a, b)` -- the type two branches both have: the one the other flows into, as `i64` for a numeric literal and an `i64`. The checker's `common_type` uses it for the branches of `if` and `case` and the elements of an array literal.

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

The MLIR code generator's entry point is `generateModules` (`include/codegen/ModuleGen.h`); `ModuleGen.cpp` builds the module, and kernels are `KernelGen`'s (`KernelGen.cpp`). It builds the module around the code; function bodies go through MIR (`MIRLowering`, `knowledge_base/plans/mir.md`).

### MIR

`MIRLowering` lowers each type-checked, monomorphized function body to MIR, a mid-level IR in the `sammine` dialect (`include/dialect/`): blocks with explicit control flow (`cf.br`, `cf.cond_br`, `cf.switch`), every name resolved, operators and `for` desugared to calls, and memory and ownership explicit in its ops (`sammine.alloc`, `sammine.move`, `sammine.lend`, ...). It dispatches on the node kind with `llvm::dyn_cast` (`MIRLowering::lowerExpr`), as each node lowers to a value. A function it can't lower is a compiler error. `sammine-mir-to-llvm` (`src/dialect/MIRToLLVM.cpp`) then lowers MIR to the LLVM dialect.

### MLIR Dialects Used

- **sammine** -- MIR: function bodies, their types and ops
- **Arith** -- arithmetic operations on integers and floats (`arith.addi`, `arith.mulf`, `arith.cmpi`, etc.)
- **Func** -- functions and calls between them (`func.func`, `func.call`, `func.return`)
- **CF** -- control flow (`cf.br`, `cf.cond_br`, `cf.switch`)
- **LLVM** -- C bindings (`llvm.func`), and what MIR lowers to
- **Linalg, Tensor, MemRef, GPU** -- kernels

### Generation Phases

`ModuleGen::generate()` runs several phases:

1. **Runtime function declarations.** Declares `malloc`, `free`, `exit` and `dprintf`, for allocation and bounds-check reports.

2. **Forward-declare functions.** Iterates over all `FuncDefAST` and `TypeClassInstanceAST` nodes to emit `func.func` symbol declarations (without bodies) in MIR types (`MIRTypes`), and C bindings. This enables mutual recursion and function-as-value references. `sammine-mir-to-llvm` converts a declaration's MIR types as it does a definition's. A C binding that is an `llvm.func` needs LLVM types already: it gets them from its MIR types through `llvmTypeOf`, the pass's own conversion, so there is one mapping from sammine types to LLVM's.

3. **MIR.** `lowerBodies` gives every function declaration its body through MIR.

4. **Linkage and kernels.** Iterates again: functions get their linkage (`setLinkage`); kernels and their wrappers are defined (`KernelGen::define`); C bindings, struct, enum, type alias and typeclass declarations have nothing more to emit, and any other definition is an error.

### Lowering Pipeline

`MLIRLowering.cpp` defines the pass pipeline that converts the multi-dialect MLIR module to pure LLVM IR:

1. `sammine-mir-to-llvm` -- lowers MIR's types and ops to the LLVM dialect
2. `SCF -> ControlFlow` -- lowers structured control flow to branches
3. `Arith -> LLVM` -- lowers arithmetic ops to LLVM dialect ops
4. `ControlFlow -> LLVM` -- lowers cf.br/cf.cond_br/cf.switch to LLVM branches
5. `Func -> LLVM` -- lowers func.func/func.call/func.return to LLVM
6. `ReconcileUnrealizedCasts` -- cleans up remaining type casts

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
   - `TypeLayout.cpp` (`layout::typeSize`)
   - `MIRTypes.cpp` (`mirType`) and the `MIRLowering*.cpp` files (lowering values of the new type), with a MIR type in `SammineTypes.td` and its conversion in `MIRToLLVM.cpp` if it needs one
   - `MIRTypes.cpp` and `SammineDialect.cpp` (`ownsMemory`), and `OwnershipPaths.cpp` (`partsOf`, `typeOf`), if the type can hold owned memory
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
9. Lower it to MIR in `MIRLowering::lowerExpr()` (an expression), or handle it in `ModuleGen::generate()` (a definition).
10. Add `to_string()` in `src/ast/AstToString.cpp` and printer support in `AstPrinterVisitor.cpp`.
11. Add parser rules in `src/Parser.cpp`.

### Adding a New Expression

1. Follow the "Adding a New AST Node" checklist above with the node in the `FirstExpr..LastExpr` range.
2. Add parsing in `Parser.cpp`. If it is an infix operator, add it to the precedence table. If it is a prefix construct (like `alloc`, `drop`, `len`), add a new parse method and call it from the appropriate entry point.
3. Implement `synthesize()` in `BiTypeCheckerSynthesize.cpp` to compute the expression's type.
4. Add a `lowerMyExpr()` method to `MIRLowering` and call it from `lowerExpr()`; if it needs new MIR ops, add them to `SammineOps.td` with their lowering in `MIRToLLVM.cpp`.

### Adding a New Definition

1. Follow the "Adding a New AST Node" checklist above with the node in the `FirstDef..LastDef` range.
2. Add parsing in `Parser.cpp`, called from the top-level definition loop.
3. In `ScopeGeneratorVisitor::preorder_walk(ProgramAST*)`, register the definition's name.
4. In `BiTypeCheckerVisitor::visit(ProgramAST*)`, add the definition to the appropriate registration pass (first pass for type defs, second for function signatures, third for full checking).
5. In `ModuleGen::generate()`, declare or emit the new definition type, or add it to the definitions with nothing to emit.
6. In `Monomorphizer`, add cloning support if the definition can be generic.
7. Consider import system implications: update `resolve_imports()` in `Compiler.cpp` if the definition should be importable/exportable.
