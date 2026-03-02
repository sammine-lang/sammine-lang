# Generic Import Follow-up TODOs

Issues and missing coverage identified by swarm verification after implementing
cross-module generic function imports.

## Cleanup

- [x] Delete stale `dummy_enum.mni` in project root (shadows real .mni files)

## Minor Code Issues

- [x] `collect_expr_type_names` in `emit_interface()` doesn't recurse into
      `IndexExprAST`, `ArrayLiteralExprAST`, `FieldAccessExprAST`,
      `TupleLiteralExprAST`, etc. A non-exported type deep in these expressions
      would only fail at import-time instead of emit-time.
- [x] Case arm bodies are not visited by the scope generator's qualification
      logic. If an imported generic contains a `case` expression referencing
      module-local names, those names won't be qualified.
      (Investigated: arm bodies ARE visited by ASTVisitor traversal; pattern
      variant names are resolved by type, not scope lookup. Added test:
      `import_generic_case.mn`.)

## Missing E2E Test Coverage

- [x] Generic calling another generic with *different* type args across modules
- [x] Generic function using an enum from the same module
- [x] Generic with array parameter types across modules
- [x] Generic with pointer parameter types across modules
- [x] Multiple instantiations of same imported generic with different types
      (e.g. `identity<i32>` and `identity<i64>` in one file)
- [x] True multi-level transitive generic imports (A imports B imports C,
      all with generics)
- [x] Generic where T is instantiated as a struct type
      (e.g. `identity<Point>(some_point)`)
