# Import System & Stdlib

## Namespace Rules
- Main file → `sammine`; library → filename stem; C functions → `C`
- All defs module-qualified by scope generator (except `main`)

## Import Resolution (`Compiler::resolve_imports`)
1. Find `.mn` file: CWD → `-I` paths → source dir → stdlib_dir
2. Diamond dedup via `imported_modules` set (canonical path)
3. Parse, register self-alias, recursive resolve (sub-imports as transitive)
4. Visibility filter: exported defs + all typeclass decls/instances (direct only)
5. Non-generic exported FuncDefAST → converted to ExternAST (importer sees declaration, not body)
6. Generic functions kept as full FuncDefAST (monomorphizer needs body)
7. Module qualification post-parse via `with_module(name)`
8. Prepend to `DefinitionVec`

## Transitive Import Rules
- Only generic function defs and type defs (structs, enums, aliases) pulled in
- Non-exposed externs skipped
- Transitive imports do NOT leak to importers

## C:: Extern Handling
- Multiple modules can declare `reuse printf(...)` — duplicates allowed
- C:: fallback: if `module::func` not found, tries `C::func`
- `reuse` syntax: no `C::` prefix in source — compiler adds it internally

## Linkable Artifacts
- Search per module: if Static → `.a` → `.so`; else `.so` → `.a`
- Directory order: CWD → `-I` → source dir → stdlib dir

## Parser Aliases
- Pre-registered: `C` → `C`, `sammine` → `sammine`
- `import math as m;` → `alias_to_module[m] = math`

## Stdlib
| File | Purpose |
|------|---------|
| `std.mn` | `export reuse printf(...)` → `std::printf` |
| `io.mn` | Typed print/println, file I/O, stdin readers, stderr |
| `str.mn` | `String` struct (data, length, cap), new/delete |
| `vec.mn` | Generic `Vec<T>` (data: 'ptr<T>, length, cap), new/push/delete; internal grow_buf |
| `definitions.mn` | Typeclasses (Sized, Add, Sub, Mul, Div, Mod, Hash, Indexer), Sized instances, `type Option<T>` |
| `io_runtime.c` | C runtime for io module |

- Stdlib dir: `<binary>/../lib/sammine/`
- `definitions.mn` loaded as stage 4 (executables only, unconditional) — NOT via `import`
- Stdlib `.mn` files copied to build dir by CMake; generic stdlib code in `STDLIB_COPIED` (can't be pre-compiled)
