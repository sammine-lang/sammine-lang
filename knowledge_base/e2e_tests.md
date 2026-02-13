# E2E Test Conventions

## Runner
Tests use LLVM's `lit` framework. Config lives in `e2e-tests/lit.cfg.py`.

## Substitutions
| Placeholder  | Expands to                        |
|-------------|-----------------------------------|
| `%sammine`  | Path to the `sammine` binary       |
| `%check`    | Path to the `SammineCheck` binary  |
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

## File Organization
Tests are organized into feature subfolders under `e2e-tests/compilables/`:

| Subfolder   | Contents                                         |
|-------------|--------------------------------------------------|
| `array/`    | Fixed-size array tests (`arr_basic`, `arr_oob`, etc.) |
| `ptr/`      | Pointer & alloc/free tests (`ptr_basic`, `alloc_basic`, etc.) |
| `arith/`    | Arithmetic & integer type tests (`arith`, `i32_*`) |
| `func/`     | Function calls, recursion, naming (`call_func`, `fib`, `even`, etc.) |
| `control/`  | Control flow (`if`)                              |
| `types/`    | Type system tests (`simple_var_types`, `simple_record`, etc.) |
| `misc/`     | General tests (`hello`, `main`, `print`, `str`, etc.) |

Other directories:
- `e2e-tests/euler/` — Project Euler solutions used as integration tests

Naming convention: `feature_variant.mn` (e.g. `ptr_nested.mn`, `alloc_type_mismatch.mn`)
