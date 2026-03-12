# Compiler Pipeline & CLI

## Full Pipeline
```
lex → parse → imports → definitions → semantics → typecheck → linear_check → dump_ast → codegen → optimize → emit_obj → emit_library → link
```

Orchestrated by `Compiler::start()` in `src/compiler/Compiler.cpp`. Each stage is a method called sequentially; if any sets `this->error = true`, subsequent stages short-circuit.

### Stage Details

- **lex**: `Lexer` → `TokenStream` (`src/lex/Lexer.cpp`)
- **parse**: `Parser` consumes `TokenStream` → `ProgramAST`. Detects `has_main` by scanning for `FuncDefAST` named `main`. Reports lexer + parser errors.
- **imports**: For each `import`, finds `.mn` source file, parses it, recursively resolves sub-imports, filters by visibility, and prepends exported definitions to `DefinitionVec`. Module qualification happens post-parse via `with_module()`. Diamond imports deduped via canonical path. See `imports.md` for details.
- **definitions**: Executables only. Parses `stdlib_dir/definitions.mn`, prepends to `DefinitionVec`.
- **semantics**: Two sub-passes: `ScopeGeneratorVisitor` (scopes, name resolution, enum variant pre-qualification, module validation, C:: fallback) → `GeneralSemanticsVisitor` (general checks)
- **typecheck**: `BiTypeCheckerVisitor` does bidirectional type checking. Monomorphized generic function/enum defs injected at front of `DefinitionVec`.
- **linear_check**: `LinearTypeChecker` checks linear type constraints.
- **dump_ast**: If `--ast-ir`, prints AST via `ASTPrinter::print()`. Exits with 1 on prior errors.
- **codegen**: If `--check`, sets state to `Finished` (exit 0, skips remaining stages). Otherwise `codegen_mlir()`: init MLIR context (Arith, Func, LLVM, SCF, CF, MemRef, Linalg, Tensor, Bufferization, Affine) + register bufferization interfaces → `mlirGen()` returns `MLIRGenResult{cpuModule, kernelModule}` → optional `--mlir-ir` dump (both modules) → `lowerMLIRToLLVMIR()` → transfer data layout/target triple. Pre-pass forward-declares all functions AND externs. See **2-Module Architecture** below.
- **optimize**: LLVM `PassBuilder` with `O2`. Handles `--llvm-ir pre/post/diff` modes.
- **emit_obj**: Emits `.o` via `TargetMachine::addPassesToEmitFile`. Respects `-O` directory.
- **emit_library**: Libraries only. Single dispatch point using typed `LibFormat` enum (parsed from `--lib` CLI string in constructor). `LibFormat::Static` → `emit_archive_impl()` (collects `.o` + transitive deps, runs `ar rcs`). `LibFormat::Shared` → `emit_shared_impl()` (shells out to `clang++ -shared`). `LibFormat::None` → skipped. Creates `GlobalIFunc` entries mapping mangled module-qualified names to underlying C-named extern functions via trivial resolver functions (IFunc bridge for library exports).
- **link**: Executables only. Links `.o` + `io_runtime.o` into `.exe` via `clang++` (fallback `g++`).

## 2-Module Architecture

When kernels are present, `mlirGen()` produces two MLIR modules:
- **cpuModule**: Standard LLVM-level ops (arith, func, llvm, scf, cf, memref)
- **kernelModule**: Pure tensor/linalg ops (created lazily on first `KernelDefAST`; null if no kernels)

Each kernel function gets three forward declarations:
1. `__kernel_<name>` in kernel module (tensor types, private)
2. `__kernel_<name>` in CPU module (memref types, private)
3. `<name>` wrapper in CPU module (public, CPU ABI)

### Lowering Flow (`lowerMLIRToLLVMIR()` in `src/codegen/MLIRLowering.cpp`)

**Kernel bufferization** (if kernel module exists):
1. `one-shot-bufferize` (tensor → memref)
2. `linalg-to-loops` (linalg ops → scf loops)
3. `buffer-hoisting` + `buffer-loop-hoisting`
4. `promote-buffers-to-stack` (64KB limit)
5. `expand-strided-metadata` + `lower-affine`
6. Merge kernel functions into CPU module (replaces memref forward-declarations)

**MLIR→LLVM lowering** (on unified module):
1. `scf-to-cf`
2. `arith-to-llvm`
3. `cf-to-llvm`
4. `finalize-memref-to-llvm`
5. `func-to-llvm`
6. `reconcile-unrealized-casts`

**Post-lowering attributes**: all functions get `nounwind`; `malloc` return gets `noalias`; kernel wrappers get `noinline` (preserves LICM).

### Bufferization Interface Registrations

Registered in `Compiler.cpp` before codegen: arith, linalg, tensor, scf, cf, func, bufferization + buffer-deallocation, memref allocation op interfaces (needed for `promote-buffers-to-stack`).

## CLI Flags

Parsed in `src/sammine.cpp` via `argparse`. Stored in `std::map<compiler_option_enum, std::string>`.

| Flag | Description |
|------|-------------|
| `-f` / `--file` | Input source file path (mutually exclusive with `-s`) |
| `-s` / `--str` | Inline source string (mutually exclusive with `-f`) |
| `-O` | Output directory for `.o`/`.exe` — defaults to cwd, created if absent |
| `-I` | Import search paths — repeatable, joined with `;` internally |
| `--check` | Stop after type checking — sets state to `Finished` (exit 0), skips codegen/optimize/emit/link |
| `--llvm-ir <pre\|post\|diff>` | Dump LLVM IR before/after/diff of optimization |
| `--mlir-ir` | Dump MLIR IR before lowering (both CPU and kernel modules), then exit |
| `--ast-ir` | Dump AST after type checking |
| `--diagnostics` | `;`-separated debug types: `dev` (C++ source location on errors), `stages` (stage entry logs), `lexer`, `parser`, etc. Default: `dev` |
| `--time [simple\|sparse\|coarse]` | `simple`: total only. `sparse`: per-phase table. `coarse`: per-phase + LLVM pass timings |
| `--lib [static]` | Emit library output. `--lib=static`: `.a` archive (bundles transitive deps). `--lib` alone: `.so` shared library. Ignored for executables |

## Import Path Resolution

Search order for `.mn` source files:
1. CWD → 2. `-I` paths (in order) → 3. Source file's parent dir → 4. `<binary_parent>/../lib/sammine/` (stdlib)

Imported `.mn` files are parsed, then definitions are module-qualified post-parse via `with_module()` and prepended to `DefinitionVec`.

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
