# E2E Test Conventions

LLVM `lit` framework. Config: `e2e-tests/lit.cfg.py`.

## Running Tests
Always use cmake targets — never run test binaries directly:
```bash
cmake --build build -j --target unit-tests e2e-tests
```

## Substitutions
| Placeholder | Expands to |
|-------------|------------|
| `%sammine` / `%check` | `sammine` / `SammineCheck` binary paths |
| `%full` / `%dir` / `%base` | Full `.mn` path / its directory / basename without `.mn` |
| `%O` / `%I` | `-O %T` / `-I %T` — output dir / import search dir flags |
| `%T` | (lit built-in) Per-test temp output dir |

## CHECK Directives
`# CHECK:` match line, `# CHECK-NEXT:` match next line, `# CHECK-ERR:` match error output, `# CHECK-NOT:` assert absence.

## Test Patterns

**Success** — compile + run + check stdout:
```
# RUN: %sammine --file %full %O && %T/%base.exe | %check %full
# CHECK: expected output line
```

**Error** — `!` → must fail, `2>&1` → capture stderr:
```
# RUN: ! %sammine --file %full %O 2>&1 | %check %full
# CHECK: Expected error message substring
```
Variants: `--diagnostics` (semantic/type errors, `compilables/`), `--check` (ariadne pretty errors, `error_reporting/`), `--diagnostics=dev` (source file/line — debug only, not in tests).

**Stderr diagnostic** — compiler succeeds but emits to stderr (e.g. `--llvm-ir pre/post/diff`):
```
# RUN: %sammine --file %full %O --llvm-ir pre 2>&1 | %check %full
```
For invalid flag values, prefix with `!`.

**Multi-file import** — compile library first → `.o` + `.mni` in `%T`:
```
# RUN: %sammine --file %dir/Inputs/math.mn %O && %sammine --file %full %O %I && %T/import_basic.exe | %check %full
```
Library sources in `Inputs/` (excluded from lit). `%dir` locates sibling files. Syntax: `import module_name as alias;`, C bindings: `reuse func(params) -> ret;`, exports: `export let`.

## File Organization

Tests in `e2e-tests/compilables/` by feature. Naming: `feature_variant.mn`.

| Subfolder | Contents |
|-----------|----------|
| `array/` | Fixed-size arrays |
| `ptr/` | Pointers & alloc/free |
| `arith/` | Arithmetic & integer types |
| `func/` | Function calls, recursion |
| `functions/` | First-class functions & partial application |
| `control/` | Control flow (if, nested_if, if_expr_value) |
| `types/` | Type system (var types, records, ...) |
| `misc/` | General tests, compiler flags |
| `generics/` | Monomorphized generics |
| `enums/` | Enum decls, case/match, payloads, generics, unqualified variants |
| `type_inference/` | Type inference, numeric literal resolution |
| `import/` | Module imports; library sources in `Inputs/` |
| `typeclass/` | Typeclass decls & instances |

Other dirs: `error_reporting/` — diagnostic rendering tests. `euler/` — Project Euler integration tests.

## README Demo
`e2e-tests/compilables/misc/readme_demo.mn` mirrors the README "Language Features" block. **Keep both in sync; verify compilation after changes.**

## Gotchas
- `if/else` as **statements** (not last expr in block) need trailing `;` — parser errors without it.
- Auxiliary inputs in `Inputs/` dirs are excluded from lit discovery.
