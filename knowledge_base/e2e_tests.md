# E2E Test Conventions

## Runner
Tests use LLVM's `lit` framework. Config lives in `e2e-tests/lit.cfg.py`.

## Substitutions
| Placeholder  | Expands to                        |
|-------------|-----------------------------------|
| `%sammine`  | Path to the `sammine` binary       |
| `%check`    | Path to the `SammineCheck` binary  |
| `%dir`      | Directory containing the current `.mn` file |
| `%full`     | Full path to the current `.mn` file (`%s`) |
| `%base`     | Basename without `.mn` extension   |

## Test Patterns

### Success test (compile + run + check stdout)
```
# RUN: %sammine --file %full && ./%base.exe | %check %full
# CHECK: expected output line
# CHECK-NEXT: next expected line
```

### Error test (expect compilation failure + check diagnostic)
```
# RUN: ! %sammine --file %full --diagnostics 2>&1 | %check %full
# CHECK: Expected error message substring
```
Key details:
- `!` before `%sammine` means the command must fail (non-zero exit)
- `--diagnostics` enables error reporting output
- `2>&1` captures stderr into the pipe so `SammineCheck` can match it

### Stderr diagnostic test (compiler flag output)
```
# RUN: %sammine --file %full --llvm-ir pre 2>&1 | %check %full
# CHECK: define i32 @main()
# CHECK: alloca i32
```
Use this pattern when testing compiler flags that emit to stderr (e.g. `--llvm-ir pre/post/diff`).
The compiler still succeeds (exit 0), but `2>&1` captures the diagnostic stderr output for CHECK matching.

For flags that should reject invalid values:
```
# RUN: ! %sammine --file %full --llvm-ir invalid 2>&1 | %check %full
# CHECK: --llvm-ir requires a value: pre, post, or diff
```

## File Organization
Tests are organized into feature subfolders under `e2e-tests/compilables/`:

| Subfolder   | Contents                                         |
|-------------|--------------------------------------------------|
| `array/`    | Fixed-size array tests (`arr_basic`, `arr_oob`, etc.) |
| `ptr/`      | Pointer & alloc/free tests (`ptr_basic`, `alloc_basic`, etc.) |
| `arith/`    | Arithmetic & integer type tests (`arith`, `i32_*`) |
| `func/`     | Function calls, recursion, naming (`call_func`, `fib`, `even`, etc.) |
| `functions/`| First-class functions & partial application (`func_as_arg`, `partial_basic`, etc.) |
| `control/`  | Control flow (`if`, `nested_if`, `if_expr_value`) |
| `types/`    | Type system tests (`simple_var_types`, `simple_record`, etc.) |
| `misc/`     | General tests, compiler flag tests (`llvm_ir_pre`, mutability, etc.) |

| `generics/` | Monomorphized generic functions (`identity`, `generic_apply`, `explicit_type_args`, etc.) |
| `import/`   | Module import tests using `reuse` keyword (`import_basic`, `import_export_let`, `no_transitive_leak`, etc.); library sources in `Inputs/` |
| `typeclass/` | Typeclass declarations & instances (`sizeof_basic`, `sizeof_struct`, `user_typeclass`, `generic_calls_typeclass`, `typeclass_ordering`, `missing_instance`) |

Other directories:
- `e2e-tests/euler/` — Project Euler solutions used as integration tests

Naming convention: `feature_variant.mn` (e.g. `ptr_nested.mn`, `alloc_type_mismatch.mn`)

### Multi-file import test
```
# RUN: %sammine --file %dir/Inputs/math.mn && %sammine --file %full && ./import_basic.exe | %check %full
# CHECK: 7
```
- Compile the library first (produces `.o` + `.mni` in CWD), then compile the importing file
- Library sources go in `Inputs/` subdirectory (excluded from lit discovery via `config.excludes`)
- Use `%dir` to locate sibling files relative to the test
- Import syntax uses `reuse` keyword (not `extern`): `reuse func_name(params) -> ret;`
- Use `export let` in library files to explicitly export user-defined functions

## README Demo
The file `e2e-tests/compilables/misc/readme_demo.mn` mirrors the code in the README's "Language Features" block.
**Every time you update the README code example, update `readme_demo.mn` to match (and vice versa), then verify it compiles and passes.**

## Gotchas
- `if/else` blocks used as **statements** (not the last expression in a block) need a trailing semicolon: `if cond { ... } else { ... };` — without it, the parser errors on the next token
- Auxiliary test inputs in `Inputs/` directories are excluded from lit discovery
