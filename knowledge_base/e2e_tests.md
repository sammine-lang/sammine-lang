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

Other dirs: `error_reporting/` — error message regression tests (39 tests). `euler/` — Project Euler integration tests.

## Error Reporting Test Coverage (`error_reporting/`)

39 tests covering error messages across compiler phases. Each test uses `# RUN: ! %sammine --file %full %O 2>&1 | %check %full` and checks error message substrings.

| Category | Tests | Error paths covered |
|----------|-------|---------------------|
| Lexer | `single_invalid`, `double_invalid`, `multi_invalid_same_line`, `wide_invalid`, `unterminated_char` | Invalid tokens, unterminated char literal |
| Type mismatches | `return_type_mismatch`, `if_branch_mismatch`, `if_cond_not_bool`, `case_arm_type_mismatch`, `var_init_mismatch`, `int_literal_to_float`, `signed_unsigned_mix` | Return type, if branches, if condition, case arms, variable init |
| Function calls | `func_too_many_args`, `func_arg_type_mismatch`, `not_callable` | Arity, arg type, non-callable |
| Immutability | `immutable_reassign`, `immutable_array_write` | Variable reassign, array index write |
| Structs | `struct_missing_field`, `struct_field_access_non_struct`, `unknown_struct_type` | Field count, field on non-struct, unknown type |
| Enums/case | `case_non_enum`, `case_non_exhaustive`, `case_multi_missing`, `case_wrong_bindings`, `case_zero_bindings`, `enum_wrong_variant`, `enum_variant_arg_count`, `invalid_backing_type`, `enum_discriminant_payload`, `int_enum_dup_discriminant`, `int_enum_mixed`, `int_enum_negative` | Exhaustiveness, variant errors, backing types |
| Linear types | `linear_branch_mismatch` | Branch consumption inconsistency |
| Generics | `generic_arg_count` | Wrong argument count for generic function |
| Operators | `operator_no_instance`, `no_instance_compare`, `bitwise_on_float`, `negate_non_numeric`, `unsigned_neg` | Missing typeclass, bitwise on float, negation |
| Scope/naming | `undeclared_variable`, `reserved_double_underscore` | Undefined var, reserved `__` |
| Main signature | `main_bad_params`, `main_wrong_argc_type` | Wrong param count/type |

Additional error tests live in `compilables/` subdirectories (ptr/, array/, linear/, generics/, func/, types/, typeclass/) — these test the same error paths with `--diagnostics` flag and are not duplicated in `error_reporting/`.

### Still-untested error paths (future work)
- Reassigning linear variable without consuming previous value
- `'name' is not callable` for non-function types (partially covered)
- Type parameter inference failure (`Type parameter 'T' could not be inferred`)
- Typeclass method not found / wrong arg count
- Variadic function arg count errors
- Most parser syntax errors (~100 `imm_error` paths)

## README Demo
`e2e-tests/compilables/misc/readme_demo.mn` mirrors the README "Language Features" block. **Keep both in sync; verify compilation after changes.**

## Gotchas
- `if/else` as **statements** (not last expr in block) need trailing `;` — parser errors without it.
- Auxiliary inputs in `Inputs/` dirs are excluded from lit discovery.
