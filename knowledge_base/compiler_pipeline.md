# Compiler Pipeline

## Stages
```
lex → parse → imports → semantics → typecheck → dump_ast → mir → codegen → optimize → emit_obj → emit_library → link
```
- Orchestrated by `Compiler::start()`. Error in any stage → `should_stop()` → skip rest.
- `--check` sets state to `Finished` (exit 0) before codegen — skips codegen/optimize/emit/link, after the `mir` stage has checked ownership
- `imports`: every module implicitly imports the prelude `definitions` (except definitions.mn itself), then its own imports
- `semantics`: ScopeGenerator → GeneralSemantics (two sub-passes)
- `typecheck`: BiTypeChecker + monomorphized def injection at front of DefinitionVec; then, if it found nothing, `OpenTypeCheck` and `ViewPlacementCheck`
- `mir`: init the MLIR dialects (Arith, Func, LLVM, SCF, CF, sammine; MemRef, Linalg, Tensor, Bufferization, Affine for kernels) + bufferization interfaces → `generateModules()` → `checkViews()` and `checkOwnership()`, their diagnostics reported through a `Reportee`
- `codegen`: `lowerMLIRToLLVMIR()` on the modules the `mir` stage built
- `optimize`: LLVM PassBuilder with O2
- `emit_library`: writes the `.so`/`.a`
- `link`: `clang++` (fallback `g++`) with `io_runtime.o`

## Library vs Executable
- `has_main` detected during parse (scan for `FuncDefAST` named "main")
- Executables: emit_obj → link. `--lib` ignored.
- Libraries: `--lib=static` → `.a` (fat, bundles transitive deps); `--lib` → `.so`

## 2-Module Architecture
- `generateModules()` → `GeneratedModules{cpuModule, kernelModule}` (kernel null if no kernels)
- Kernel module bufferized separately → merged into CPU module → unified MLIR→LLVM lowering
- See `codegen.md` for full lowering pipeline

## Import Path Resolution
1. CWD → 2. `-I` paths → 3. source file parent dir → 4. `<binary>/../lib/sammine/` (stdlib)
- Names: the import resolver only records each definition's `defining_module`; `NameResolver::use_program` (scope generation) names every definition after it and resolves every reference. Later stages only read names
- Diamond dedup via canonical path in `imported_modules` set

## Error Reporting
- `Location`: byte-offset pair. `|` spans locations. `NonPrintable()` = `(-1,-1)` suppresses rendering.
- `Reportee`: base class for visitors. `add_error()`/`add_warn()`/`add_diagnostics()` auto-capture C++ source_location. `abort()` for ICEs with cpptrace.
- `Reporter`: groups by location, renders Unicode waterfall art, TTY-aware coloring. `--diagnostics=dev` appends C++ source location.

## LLVMRes
- Shared LLVM resources: LLVMContext, Module, TargetMachine, SammineJIT
- Module replaced by MLIR-lowered module in `codegen_mlir()` (data layout/triple copied)
