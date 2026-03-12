# Import System & Stdlib

## Namespace Rules

| Context | Namespace |
|---------|-----------|
| Main file (has `main()`) | `sammine` |
| Library file (no `main()`) | filename stem (e.g. `math.mn` → `math`) |
| C functions | `C` (explicit `C::` prefix required) |

All definitions are module-qualified by `ScopeGeneratorVisitor` (except `main`). QualifiedName uses `::` separator: `sammine::add`, `math::multiply`, `C::printf`.

## QualifiedName

`QualifiedName` stores a `std::vector<std::string> parts_`:
- `QualifiedName("add")` → `["add"]` (single-arg constructor does NOT split on `::`)
- `QualifiedName::qualified("math", "add")` → `["math", "add"]`
- `QualifiedName::from_parts({"math","Color","Red"})` → `["math", "Color", "Red"]`

Key methods: `get_name()` (last part), `get_module()` (first part if >1), `get_qualifier()` (all except last), `is_qualified()`, `is_c_namespace()`, `mangled()` (join with `::`), `with_module(mod)` (prepend), `prepend(seg)`.

Critical: `QualifiedName(string)` stores the entire string as one element. Typeclass mangling like `"Add::i32::add"` stays as one opaque part.

## Declaration Syntax

| Keyword | Purpose | Visibility |
|---------|---------|------------|
| `reuse func(...)` | Bind a C extern function | Module-private |
| `export reuse func(...)` | Bind + export a C extern | Exported to importers |
| `export let func(...)` | Export user-defined function | Exported to importers |
| `let func(...)` (no export) | Module-private function | NOT exported |
| `export struct Name { ... }` | Export struct type | Exported to importers |
| `export type Name = V1 \| V2` | Export enum type | Exported to importers |
| `export type Alias = ...` | Export type alias | Exported to importers |

```
reuse printf(fmt: ptr<char>, ...) -> i32;              # private C binding
export reuse printf(fmt: ptr<char>, ...) -> i32;       # exported — importers use std::printf(...)
export let add(a: i32, b: i32) -> i32 { a + b }       # exported as <module>::add
```

Note: the `reuse` keyword does NOT use a `C::` prefix in source syntax. The `C::` namespace is added by the compiler internally for scope resolution.

## Parse-on-the-Fly Imports

Imports are resolved by parsing `.mn` source files directly at compile time. No separate compilation step or `.mni` files needed.

### Import Resolution (`Compiler::resolve_imports`)

1. **Find `.mn` file**: Search CWD → `-I` paths → source dir → `stdlib_dir`
2. **Diamond dedup**: Module name checked against `imported_modules` set
3. **Parse**: Create `Lexer` + `Parser`; register `alias_to_module[name] = name` for self-references
4. **Recursive resolve**: Process the imported file's own imports (as transitive)
5. **Visibility filter**: Only inject definitions with `is_exported == true` + all typeclass decls/instances (direct imports only)
6. **Module qualification**: Post-parse, names are qualified via `with_module(name)` in Compiler.cpp (e.g., `add` → `math::add`)
7. **Non-generic export conversion**: Non-generic exported `FuncDefAST` entries are converted to `ExternAST` during injection (Compiler.cpp lines ~337-344) — the importer only sees a declaration, not the function body. Generic functions are inlined as full `FuncDefAST` so the monomorphizer can instantiate them.
8. **Inject**: Prepend filtered definitions at front of main `programAST->DefinitionVec`

### Module Qualification (post-parse, not in Parser)

The `Parser` constructor accepts a `default_namespace` parameter, but it is **not used** — the parameter is never stored or referenced in the constructor body (`include/parser/Parser.h` lines 203-208). Module qualification happens entirely post-parse in `Compiler::resolve_imports()` via `with_module(name)`:
- `let add(...)` in `math.mn` → prototype's `functionName.with_module("math")` → `math::add`
- `struct String { ... }` in `str.mn` → `struct_name.with_module("str")` → `str::String`
- Enum names and extern names are similarly qualified with `with_module(name)`

### ScopeGenerator Module Tracking

`ScopeGeneratorVisitor::visit(FuncDefAST*)` temporarily swaps `moduleName` to the function's module when processing imported function bodies. This ensures unqualified names are qualified correctly (e.g., `String { ... }` inside `str::new` → `str::String`, not `sammine::String`).

The `insideImportedFunc_` flag is set during imported function body processing. This suppresses "Module X is not imported" errors for code that references the imported module's own transitive imports (which were already resolved).

### C:: Extern Handling

- Multiple modules can declare `reuse printf(...)` — ScopeGenerator silently allows duplicate C:: extern registrations
- When `module::func` is not found in scope, the C:: fallback rewrites it to `C::func` if that exists (handles `std::printf` → `C::printf`)
- Codegen forward-declares all `ExternAST` definitions in the pre-pass (alongside `FuncDefAST` forward declarations)

## Linkable Artifact Resolution

When resolving imports, the compiler searches for linkable artifacts per module. Search order per directory depends on `lib_format_`: if `Static`, search `.a` → `.so`; otherwise `.so` → `.a` (first match wins). Directory search order: CWD → `-I` paths → source dir → stdlib dir.

This allows library modules to emit `.a` (static archives bundling transitive deps) or `.so` (shared libraries). See `--lib` flag in `compiler_pipeline.md`.

## Multi-File Workflow

```bash
# Single-step compilation — compiler finds and parses imports on the fly
./build/bin/sammine -f main.mn -I path/to/modules

# Compile a library as a static archive (bundles transitive deps)
./build/bin/sammine -f math.mn -O build/ --lib=static    # produces math.o, math.a, math.mni

# Compile a library as a shared library
./build/bin/sammine -f math.mn -O build/ --lib            # produces math.o, math.so, math.mni
```
```
import math;
import std;

let main() -> i32 {
  std::printf("%d\n", math::add(3, 4));
  return 0;
}
```

## Stdlib

| File | Contents |
|------|----------|
| `stdlib/std.mn` | `export reuse printf(fmt: ptr<char>, ...) -> i32;` — provides `std::printf` |
| `stdlib/io.mn` | Typed print helpers (`print`, `println`, `print_i32`, `print_i64`, `print_f64`, `print_char`, `print_bool`), file I/O (`open`, `close`, `file_read_line`, `file_write`, `read_file`, `write_file`, `append_file`), stdin readers (`read_line`, `read_i32`, `read_i64`, `read_f64`), stderr (`eprint`, `eprintln`) |
| `stdlib/str.mn` | `String` struct (`data: ptr<char>`, `length: i64`, `cap: i64`), `new`/`delete` functions |
| `stdlib/vec.mn` | Generic `Vec<T>` struct (`data: 'ptr<T>`, `length: i64`, `cap: i64`), exported `new<T>`, `push<T>`, `delete<T>`; internal `grow_buf<T>` for capacity doubling |
| `stdlib/definitions.mn` | Built-in typeclasses and `Option<T>` type alias (see below) |
| `stdlib/io_runtime.c` | C runtime for io module (compiled to `io_runtime.o`) |

Stdlib `.mn` files are copied to `build/lib/sammine/` by CMake. The compiler locates them via `stdlib_dir` (relative to the binary: `<binary>/../lib/sammine/`).

### `definitions.mn` Details

Loaded automatically as **stage 4** (`load_definitions`) for executables only — runs after import resolution (stage 3). Not imported via `import definitions;`; contents are prepended unconditionally to `programAST->DefinitionVec`. See [Pipeline Stage Cross-Reference](#pipeline-stage-cross-reference).

**Typeclasses:**

| Typeclass | Signature | Purpose |
|-----------|-----------|---------|
| `Sized<T>` | `Sized() -> i64` | Returns byte size of `T` |
| `Add<T>` | `Add(a: T, b: T) -> T` | Addition |
| `Sub<T>` | `Sub(a: T, b: T) -> T` | Subtraction |
| `Mul<T>` | `Mul(a: T, b: T) -> T` | Multiplication |
| `Div<T>` | `Div(a: T, b: T) -> T` | Division |
| `Mod<T>` | `Mod(a: T, b: T) -> T` | Modulo |
| `Hash<T>` | `Hash(val: T) -> u64` | Hashing |
| `Indexer<T, E>` | `Indexer(coll: T, idx: i64) -> E` | Collection indexing (two type params) |

**Sized instances:**

| Type | Size (bytes) |
|------|-------------|
| `i32` | 4 |
| `i64` | 8 |
| `f64` | 8 |
| `bool` | 1 |
| `char` | 1 |

**Type alias:** `type Option<T> = Some(T) | None;` — a generic enum alias available to all executables.

## Import Isolation

- Non-exported functions/structs/enums are filtered out during import
- Exported `ExternAST` entries (i.e., `export reuse` declarations) are included for direct imports; non-exposed externs and all externs in transitive imports are skipped
- Imported modules' transitive imports do NOT leak to importers — `known_modules` only includes direct imports
- For transitive imports, only generic function defs and type defs (structs, enums, type aliases) are pulled in — needed for monomorphization
- Tests: `e2e-tests/compilables/import/no_transitive_leak.mn`, `export_let_mni.mn`

## Parser Aliases

Pre-registered aliases:
- `C` → `C` (always available for C extern access)
- `sammine` → `sammine` (reserved namespace)
- For imported modules, `alias_to_module[name] = name` is set directly in `Compiler::resolve_imports()` before parsing

Import aliases: `import math as m;` → `m::add(...)` resolves to `math::add`

Multi-level `::` parsing: `ParseCallExpr` and `ParseCaseExpr` consume chains like `math::Color::Red(x)` via a loop, resolving the first part through `alias_to_module`.

## Pipeline Stage Cross-Reference

| Stage | Method | Description |
|-------|--------|-------------|
| 1 | `lex()` | Tokenize source |
| 2 | `parse()` | Parse tokens into AST |
| 3 | `resolve_imports()` | Parse-on-the-fly import resolution |
| 4 | `load_definitions()` | Prepend `stdlib/definitions.mn` (executables only, unconditional) |
| 5 | `semantics()` | ScopeGenerator + GeneralSemantics |
| 6 | `typecheck()` | BiTypeChecker + Monomorphizer |
| 7 | `linear_check()` | Linear type checking (`'ptr<T>` must-use) |
| 8 | `dump_ast()` | Optional AST dump (`--ast-ir`) |
| 9 | `codegen()` | MLIR generation + lowering to LLVM IR |
| 10 | `optimize()` | LLVM O2 pipeline |
| 11 | `emit_object()` | Emit `.o` file |
| 12 | `emit_library()` | Emit `.a` or `.so` (library modules only) |
| 13 | `link()` | Link into executable (executables only) |
