# Generic Import Follow-up TODOs

Issues and missing coverage identified by swarm verification after implementing
cross-module generic function imports.

## Cleanup

- [ ] Delete stale `dummy_enum.mni` in project root (shadows real .mni files)

## Minor Code Issues

- [ ] `collect_expr_type_names` in `emit_interface()` doesn't recurse into
      `IndexExprAST`, `ArrayLiteralExprAST`, `FieldAccessExprAST`,
      `TupleLiteralExprAST`, etc. A non-exported type deep in these expressions
      would only fail at import-time instead of emit-time.
- [ ] Case arm bodies are not visited by the scope generator's qualification
      logic. If an imported generic contains a `case` expression referencing
      module-local names, those names won't be qualified.

## Missing E2E Test Coverage

- [ ] Generic calling another generic with *different* type args across modules
- [ ] Generic function using an enum from the same module
- [ ] Generic with array parameter types across modules
- [ ] Generic with pointer parameter types across modules
- [ ] Multiple instantiations of same imported generic with different types
      (e.g. `identity<i32>` and `identity<f64>` in one file)
- [ ] True multi-level transitive generic imports (A imports B imports C,
      all with generics)
- [ ] Generic where T is instantiated as a struct type
      (e.g. `identity<Point>(some_point)`)
