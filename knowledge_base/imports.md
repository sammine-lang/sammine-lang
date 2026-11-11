# Import System & Stdlib

## Namespace Rules
- Main file → `sammine`; library → filename stem; C functions → `C`
- All defs module-qualified by scope generator (except `main`)

## Import Resolution (`Compiler::resolve_imports`)
1. Find `.mn` file: CWD → `-I` paths → source dir → stdlib_dir
2. Diamond dedup via `imported_modules` set (canonical path)
3. Parse, record the module's import aliases, trace its library (before its imports', for static link order), then import its own imports the same way
4. Every definition comes, private ones too (`is_exported` kept): functions, C bindings, instances, types, typeclass decls
5. Bodies stay in the module (compiled first): a non-generic FuncDefAST becomes an ExternAST. Generic functions keep their bodies (compiled here). An instance becomes `is_declaration` but keeps its method bodies: type-checked here (as its module's code: methods get its `defining_module`) and emitted `available_externally` + `inlinehint`, so they inline but the module's copy is the one linked
6. Privacy is a naming rule: `NameResolver::check_visible` rejects a private function or type named outside its module (owners table from `use_program`); the type checker rejects a private instance (`private_instances`, `has_instance` compares `enclosing_function->defining_module`, which generic instantiations inherit). A module's own code compiled here (its generic functions) may use both
7. Names: only `defining_module` is set here; `NameResolver` names the definitions
8. Prepend to `DefinitionVec`

## Modules Reached Through Other Modules
- Imported exactly like the program's own imports, whichever reaches a module first (no reduced "transitive" form), and linked: a module's code compiled here (its generic functions, its instances' copies) may use them
- The program can't name them: naming needs an import alias of its own, checked by NameResolver ("Module 'std' is not imported")

## C Bindings (`reuse`)
- In source, an imported module's binding is named like its other definitions (`std::printf`); its symbol is always its C name (`printf`). Codegen declares that symbol once, however many modules bind it, and calls it directly: no bridge in the library
- Imported like any definition, private ones too, so a module's generic code compiled elsewhere can call its private bindings; naming a private one from outside is an error

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
| `str.mn` | `String` struct (data, length, cap), new/delete, `compare`, and `Less`/`Greater`/`LessEqual`/`GreaterEqual` instances on `ptr<String>` (views: an instance on `String` itself would own, and have to free, its operands). No `Equal`: `ptr<T>` has a built-in address `==` |
| `vec.mn` | Generic `Vec<T>` (data: 'ptr<T>, length, cap), new/push/delete; internal grow_buf |
| `definitions.mn` | The prelude: typeclasses (Sized, Add, Sub, Mul, Div, Mod, Convert, Hash, Indexer, Iterator), exported Sized/Iterator<Range> instances, `type Option<T>`, `struct Range<T>`. Compiled to definitions.so/.a first; other stdlib modules depend on it |
| `io_runtime.c` | C runtime for io module |

- Stdlib dir: `<binary>/../lib/sammine/`
- Prelude: `Resolver::Resolve` imports `definitions` into every module (libraries too) unless compiling definitions.mn itself. Its names get no module prefix (`NameResolver::kPrelude`, the one exception, in `qualify`), in its own library too, so `Iterator<Range<i32>, i32>::next` matches on both sides
- `for` calls `Iterator<Range<T>, T>::next`, defined in definitions.so; the importer's `available_externally` copy is inlined, so loops cost what they did when the prelude was compiled into each program
- Stdlib `.mn` files copied to build dir by CMake; generic stdlib code in `STDLIB_COPIED` (can't be pre-compiled)
