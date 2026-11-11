# Import System & Stdlib

## Namespace Rules
- Main file → `sammine`; library → filename stem; C functions → `C`
- All defs module-qualified by scope generator (except `main`)

## Import Resolution (`Compiler::resolve_imports`)
1. Find `.mn` file: CWD → `-I` paths → source dir → stdlib_dir
2. Diamond dedup via `imported_modules` set (canonical path)
3. Parse, record the module's import aliases, recursive resolve (sub-imports as transitive)
4. Every definition comes, private ones too (`is_exported` kept): functions, instances, types, typeclass decls. Only C bindings come only if exported. Transitive imports bring generic functions and types
5. Bodies stay in the module (compiled first): a non-generic FuncDefAST becomes an ExternAST, an instance becomes `is_declaration` with its method bodies emptied. Generic functions keep their bodies (compiled here)
6. Privacy is a naming rule: `NameResolver::check_visible` rejects a private function or type named outside its module (owners table from `use_program`); the type checker rejects a private instance (`private_instances`, `has_instance` compares `enclosing_function->defining_module`, which generic instantiations inherit). A module's own code compiled here (its generic functions) may use both
7. Names: only `defining_module` is set here; `NameResolver` names the definitions
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

## Import Aliases
- `import math as m;` → `m` means math; `import math;` → `math` means math; a module's own name means itself (`AST::import_aliases`)
- Kept per module in `ProgramAST::module_aliases` (the program's by Compiler::parse, each import's by the import resolver), so `NameResolver` reads a name with the aliases of the module that wrote it

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
