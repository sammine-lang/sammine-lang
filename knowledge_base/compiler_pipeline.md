# Compiler Pipeline & CLI

## Full Pipeline
```
lex → parse → build_cst → dump_cst → resolve_imports → load_definitions → semantics → typecheck → linear_check → dump_ast → codegen → optimize → emit_object → link
```

Orchestrated by `Compiler::start()` in `src/compiler/Compiler.cpp`. Each stage is a method called sequentially; if any sets `this->error = true`, subsequent stages short-circuit.

### Stage Details

- **lex**: `Lexer` → `TokenStream` (`src/lex/Lexer.cpp`)
- **parse**: `Parser` consumes `TokenStream` → `ProgramAST`. Detects `has_main` by scanning for `FuncDefAST` named `main`. Reports lexer + parser errors.
- **build_cst**: Builds a lossless Concrete Syntax Tree from `Lexer::getRawTokens()` (includes trivia: whitespace, newlines, comments). Flat structure: single `SourceFile` node with all tokens as children. Uses `TreeBuilder` + `GreenInterner`.
- **dump_cst**: If `--cst-ir`, dumps CST structure and verifies lossless round-trip (`root.text() == source`).
- **resolve_imports**: For each `import`, finds `.mn` source file, parses it on the fly with `default_namespace`, recursively resolves sub-imports, filters by visibility, and prepends exported definitions to `DefinitionVec`. Diamond imports are deduped via canonical path. See `imports.md` for details.
- **load_definitions**: Executables only. Parses `stdlib_dir/definitions.mn`, prepends to `DefinitionVec`.
- **semantics**: Two sub-passes: `ScopeGeneratorVisitor` (scopes, name resolution, enum variant pre-qualification, module validation, C:: fallback) → `GeneralSemanticsVisitor` (general checks)
- **typecheck**: `BiTypeCheckerVisitor` does bidirectional type checking. Monomorphized generic function/enum defs injected at front of `DefinitionVec`.
- **linear_check**: `LinearTypeChecker` checks linear type constraints.
- **dump_ast**: If `--ast-ir`, prints AST via `ASTPrinter::print()`. Exits with 1 on prior errors.
- **codegen**: If `--check`, exits 0. Otherwise `codegen_mlir()`: init MLIR context (Arith, Func, LLVM, SCF, CF, MemRef) → `mlirGen()` → optional `--mlir-ir` dump → `lowerMLIRToLLVMIR()` → transfer data layout/target triple. Pre-pass forward-declares all functions AND externs.
- **optimize**: LLVM `PassBuilder` with `O2`. Handles `--llvm-ir pre/post/diff` modes.
- **emit_object**: Emits `.o` via `TargetMachine::addPassesToEmitFile`. Respects `-O` directory.
- **emit_library**: Libraries only. Single dispatch point using typed `LibFormat` enum (parsed from `--lib` CLI string in constructor). `LibFormat::Static` → `emit_archive_impl()` (collects `.o` + transitive deps, runs `ar rcs`). `LibFormat::Shared` → `emit_shared_impl()` (shells out to `clang++ -shared`). `LibFormat::None` → skipped.
- **link**: Executables only. Links `.o` + `io_runtime.o` into `.exe` via `clang++` (fallback `g++`).

## CLI Flags

Parsed in `src/sammine.cpp` via `argparse`. Stored in `std::map<compiler_option_enum, std::string>`.

| Flag | Description |
|------|-------------|
| `-f` / `--file` | Input source file path (mutually exclusive with `-s`) |
| `-s` / `--str` | Inline source string (mutually exclusive with `-f`) |
| `-O` | Output directory for `.o`/`.exe` — defaults to cwd, created if absent |
| `-I` | Import search paths — repeatable, joined with `;` internally |
| `--check` | Stop after type checking, no codegen |
| `--llvm-ir <pre\|post\|diff>` | Dump LLVM IR before/after/diff of optimization |
| `--mlir-ir` | Dump MLIR IR before lowering, then exit |
| `--cst-ir` | Dump CST structure and verify lossless round-trip |
| `--ast-ir` | Dump AST after type checking |
| `--diagnostics` | `;`-separated debug types: `dev` (C++ source location on errors), `stages` (stage entry logs), `lexer`, `parser`, etc. Default: `none` |
| `--time [simple\|sparse\|coarse]` | `simple`: total only. `sparse`: per-phase table. `coarse`: per-phase + LLVM pass timings |
| `--lib [static]` | Emit library output. `--lib=static`: `.a` archive (bundles transitive deps). `--lib` alone: `.so` shared library. Ignored for executables |

## Import Path Resolution

Search order for `.mn` source files:
1. CWD → 2. `-I` paths (in order) → 3. Source file's parent dir → 4. `<binary_parent>/../lib/sammine/` (stdlib)

Imported `.mn` files are parsed with `default_namespace` set to the module name. Exported definitions are prepended to `DefinitionVec`.

## Library vs Executable Compilation

- **Detection**: `has_main` — set during parse by scanning for `FuncDefAST` named `main`
- **Executables**: `load_definitions` → `emit_object` (`<stem>.o`) → `link` (`<stem>.exe` via `clang++`/`g++`). `--lib` is ignored.
- **Libraries** (no `main`):
  - Default (no `--lib`): `emit_object` (`<stem>.o`) only
  - `--lib=static`: `emit_object` → `emit_library` → `emit_archive_impl()` (`<stem>.a`, fat archive with transitive deps)
  - `--lib` / `--lib=shared`: `emit_object` → `emit_library` → `emit_shared_impl()` (`<stem>.so`)

## LLVMRes (`include/codegen/LLVMRes.h`)

Shared LLVM resources initialized once per compilation. Contains `LLVMContext`, `Module`, `TargetMachine`, `legacy::PassManager`, `SammineJIT`. `Module` is created by `LLVMRes`, then replaced by MLIR-lowered module in `codegen_mlir()` (data layout/target triple copied from original). The old `IRBuilder` member was removed (only needed by the deleted LLVM IR backend).

## Error Reporting System

**Location** (`include/util/Utilities.h`): byte-offset pair (`source_start`, `source_end`). Default `(0,0)`, `NonPrintable()` = `(-1,-1)` suppresses rendering. `|` operator spans locations (min start, max end). `advance()`/`devance()` adjust `source_end`.

**Reportee** (`include/util/Utilities.h`): base class for error-producing visitors. `Report` = `(Location, vector<string>, ReportKind, source_location)`. Kinds: `error`/`warn`/`diag`. Methods: `add_error()`/`add_warn()`/`add_diagnostics()` — auto-capture `source_location`. `abort()`/`abort_on()`/`abort_if_not()` for ICEs (prints stack trace via cpptrace).

**Reporter** (`src/util/Utilities.cpp`): rendering engine with `file_name`, `input`, `context_radius`, `dev_mode`. Pipeline: group reports by `(Location, ReportKind)` → cluster overlapping line ranges → render with bold highlighting + Unicode waterfall art (`---┬` carets, `│` pipes, `╰──` arrows). Single error → `file:line:col` header; multiple → `file` only. TTY-aware coloring via `stderr_is_tty()`. Dev mode appends C++ source location to each error. `immediate_error()`/`immediate_warn()` bypass grouping.

## Logging (`include/util/Logging.h`)

`LOG({...})` macro — executes block if `DEBUG_TYPE` is in enabled list. Each file defines `#define DEBUG_TYPE "something"`. Enabled via `--diagnostics=stages;lexer;parser`.

## Compiler Options Enum (`include/compiler/Compiler.h`)

```
FILE, STR, LLVM_IR, AST_IR, CST_IR, DIAGNOSTIC, CHECK, TIME, ARGV0, MLIR_IR, OUTPUT_DIR, IMPORT_PATHS, LIB_FORMAT
```
Note: `BACKEND` was removed (MLIR is now the only backend).

All stored as `std::string`. Boolean flags use `"true"`/`"false"` strings.
