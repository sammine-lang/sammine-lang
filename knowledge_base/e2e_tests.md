# E2E Tests

LLVM `lit` framework. Config: `e2e-tests/lit.cfg.py`. Run: `cmake --build build -j --target e2e-tests`

## Substitutions
| Placeholder | Expands to |
|---|---|
| `%sammine` / `%check` | sammine / SammineCheck binary paths |
| `%full` / `%dir` / `%base` | Full .mn path / directory / basename without .mn |
| `%O` / `%I` / `%T` | `-O %T` / `-I %T` / per-test temp dir |

## Test Patterns
- **Success**: `# RUN: %sammine --file %full %O && %T/%base.exe | %check %full`
- **Error**: `# RUN: ! %sammine --file %full %O 2>&1 | %check %full`
- **IR check**: `# RUN: %sammine --file %full %O --llvm-ir pre 2>&1 | %check %full`
- **Multi-file**: compile library first to `%T`, then main with `%I`

## Feature Directories (`e2e-tests/compilables/`)
| Dir | Focus |
|---|---|
| `arith/` | Arithmetic, integer types |
| `array/` | Fixed-size arrays |
| `control/` | if/else, while |
| `enums/` | Enum decls, case/match, payloads, generics, int-backed |
| `func/` | Function calls, recursion |
| `functions/` | First-class functions, partial application, closures |
| `generics/` | Monomorphized generics |
| `import/` | Module imports (library sources in `Inputs/`) |
| `io/` | print, println, eprint, file I/O, linear file handles, stdin |
| `kernel/` | map, reduce, 2-module codegen, kernel_no_malloc golden test |
| `linear/` | ~59 tests: alloc/free, consumption, branches, loops, struct/array/enum fields. `break_` prefix = expected-failure |
| `llvm_ir/` | LLVM IR patterns via `--llvm-ir pre/post` + FileCheck |
| `misc/` | General tests, compiler flags, readme_demo |
| `ptr/` | Pointers, alloc/free |
| `type_inference/` | Numeric literal resolution |
| `typeclass/` | Typeclass decls & instances |
| `types/` | Type system (records, etc.) |
| `vec/` | Vec<T> stdlib: new, push, delete, linear ownership |

Other: `error_reporting/` (39 error message regression tests), `euler/` (Project Euler integration tests)

## Gotchas
- `if/else` as statements need trailing `;`
- Auxiliary inputs in `Inputs/` dirs excluded from lit discovery
- `readme_demo.mn` must stay in sync with README
