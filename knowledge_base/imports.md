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
| `reuse C::func(...)` | Bind a C extern function | Module-private |
| `export reuse C::func(...)` | Bind + export a C extern | Exported to importers |
| `export let func(...)` | Export user-defined function | Exported to importers |
| `let func(...)` (no export) | Module-private function | NOT exported |
| `export struct Name { ... }` | Export struct type | Exported to importers |
| `export type Name = V1 \| V2` | Export enum type | Exported to importers |
| `export type Alias = ...` | Export type alias | Exported to importers |

```
reuse C::printf(fmt: ptr<char>, ...) -> i32;              # private C binding
export reuse C::printf(fmt: ptr<char>, ...) -> i32;       # exported — importers use std::printf(...)
export let add(a: i32, b: i32) -> i32 { a + b }          # exported as <module>::add
```

## Parse-on-the-Fly Imports

Imports are resolved by parsing `.mn` source files directly at compile time. No separate compilation step or `.mni` files needed.

### Import Resolution (`Compiler::resolve_single_import`)

1. **Find `.mn` file**: Search CWD → `-I` paths → source dir → `stdlib_dir`
2. **Diamond dedup**: Canonical path checked against `already_imported_` set
3. **Cycle detection**: Check `currently_importing_` set → error if circular
4. **Parse**: Create `Lexer` + `Parser` with `default_namespace = module_name`
5. **Recursive resolve**: Process the imported file's own imports
6. **Visibility filter**: Only inject definitions with `is_exported == true` + all `ExternAST` (C bindings) + all typeclass decls/instances
7. **Inject**: Prepend filtered definitions at front of main `programAST->DefinitionVec`

### Parser `default_namespace`

When parsing an imported `.mn` file, the `Parser` receives the module name as `default_namespace`. This qualifies definition names (functions, structs, enums, type aliases) with the module prefix during parsing:
- `let add(...)` in `math.mn` → `math::add`
- `struct String { ... }` in `str.mn` → `str::String`
- `main` is never qualified
- C:: names are not overridden

### ScopeGenerator Module Tracking

`ScopeGeneratorVisitor::visit(FuncDefAST*)` temporarily swaps `moduleName` to the function's module when processing imported function bodies. This ensures unqualified names are qualified correctly (e.g., `String { ... }` inside `str::new` → `str::String`, not `sammine::String`).

The `insideImportedFunc_` flag is set during imported function body processing. This suppresses "Module X is not imported" errors for code that references the imported module's own transitive imports (which were already resolved).

### C:: Extern Handling

- Multiple modules can declare `reuse C::printf(...)` — ScopeGenerator silently allows duplicate C:: extern registrations
- When `module::func` is not found in scope, the C:: fallback rewrites it to `C::func` if that exists (handles `std::printf` → `C::printf`)
- Codegen forward-declares all `ExternAST` definitions in the pre-pass (alongside `FuncDefAST` forward declarations)

## Linkable Artifact Resolution

When resolving imports, the compiler searches for linkable artifacts per module. Search order per directory: `.so` → `.a` → `.o` (first match wins). Directory search order: CWD → `-I` paths → source dir → stdlib dir.

This allows library modules to emit `.a` (static archives bundling transitive deps) or `.so` (shared libraries) instead of bare `.o` files. See `--lib` flag in `compiler_pipeline.md`.

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
| `stdlib/std.mn` | `export reuse C::printf(...)` → provides `std::printf` |
| `stdlib/io.mn` | Typed print helpers (`print`, `println`, `print_i32`, etc.), file I/O, stdin readers |
| `stdlib/str.mn` | `String` struct, `new`/`delete` functions |
| `stdlib/definitions.mn` | Built-in typeclasses: `Sized<T>` with instances for `i32`(4), `i64`(8), `f64`(8), `bool`(1) |
| `stdlib/io_runtime.c` | C runtime for io module (compiled to `io_runtime.o`) |

Stdlib `.mn` files are copied to `build/lib/sammine/` by CMake. The compiler locates them via `stdlib_dir` (relative to the binary: `<binary>/../lib/sammine/`).

## Import Isolation

- Non-exported functions/structs/enums are filtered out during import
- C:: externs are always included (exported code may depend on them)
- Imported modules' transitive imports do NOT leak to importers — `known_modules` only includes direct imports
- Tests: `e2e-tests/compilables/import/no_transitive_leak.mn`, `export_let_mni.mn`

## Parser Aliases

Pre-registered aliases:
- `C` → `C` (always available for C extern access)
- `sammine` → `sammine` (reserved namespace)
- `default_namespace` → registered if non-empty (for imported files)

Import aliases: `import math as m;` → `m::add(...)` resolves to `math::add`

Multi-level `::` parsing: `ParseCallExpr` and `ParseCaseExpr` consume chains like `math::Color::Red(x)` via a loop, resolving the first part through `alias_to_module`.
