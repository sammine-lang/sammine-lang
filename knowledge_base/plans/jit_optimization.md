# Plan: JIT Performance Optimization

## Context

After adding `--jit` mode, profiling with `--time sparse` on `readme_demo.mn` (~110 lines) shows:

```
Phase                Time(ms)       %
lex                      0.01    0.0%
parse                    3.25   17.0%
imports                  0.08    0.4%
definitions              0.46    2.4%
semantics                0.16    0.8%
typecheck                1.65    8.6%
codegen                  5.30   27.7%
optimize                 3.28   17.1%
jit_execute              4.88   25.5%
total                   19.15  100.0%
```

The top 4 hotspots are: **codegen** (27.7%), **jit_execute** (25.5%), **optimize** (17.1%), **parse** (17.0%).

## Investigation Areas

### 1. Parse stage (17% — 3.25ms for 110 lines)

**Why it might be slow:**
- 75 `make_unique` calls in `src/Parser.cpp` — each is a heap allocation for an AST node
- `reporter.report(*lexer)` and `reporter.report(psr)` are called inside the timed parse stage — error rendering shouldn't be in the hot path
- Parser constructor (`Parser(tokStream, reporter, mod_name)`) may do non-trivial init

**Investigation steps:**
1. Profile with `perf record` / `perf report` to see where in parse() time is spent
2. Check if `reporter.report()` contributes meaningful time when there are no errors (the readme_demo has ~48 `#` comment warnings — oh wait, we just fixed those, so this should drop)
3. Check if the `unique_ptr<AST>` allocation pattern can use an arena/bump allocator
4. Check if `TokenStream` uses `std::vector` with many small reallocations

**Potential fixes:**
- Move `reporter.report()` calls outside the timed parse stage (or at least skip when no messages)
- Use an arena allocator for AST nodes (big win if allocation-heavy)
- Pool or reserve token stream capacity

### 2. Optimize stage (17.1% — 3.28ms)

**Why it's slow:**
- Running full O2 pipeline for JIT mode is overkill — JIT prioritizes startup latency over peak throughput
- O2 includes expensive passes: inlining, loop vectorization, GVN, etc.

**Fix:**
- Use O0 or O1 in JIT mode. This is the lowest-hanging fruit.
- In `Compiler::optimize()`, check `compiler_options[JIT] == "true"` and use `OptimizationLevel::O0` or `O1`
- Could also add `--jit-opt={0,1,2}` flag for user control

### 3. Codegen stage (27.7% — 5.30ms)

**Why it's slow:**
- MLIR dialect registration (10 dialects + bufferization interfaces) happens every compilation
- `mlirGen()` walks the full AST and builds MLIR
- `lowerMLIRToLLVMIR()` runs multiple conversion passes

**Investigation steps:**
1. Profile to split time between: dialect init, mlirGen, and MLIR→LLVM lowering
2. Check if dialect/registry setup can be cached across compilations (relevant for future REPL/watch mode)
3. Check if unused dialects can be skipped for simple programs (e.g., skip tensor/linalg/bufferization if no kernel defs)

**Potential fixes:**
- Skip kernel-related dialects when there are no kernel definitions (linalg, tensor, bufferization, affine, memref)
- Cache MLIRContext and dialect registration (for future multi-file JIT)

### 4. JIT execute stage (25.5% — 4.88ms)

**Why it's slow:**
- ORC JIT compiles the entire module eagerly (all functions compiled before main runs)
- `DynamicLibrarySearchGenerator` setup and symbol resolution
- Loading `.so` libraries via dlopen

**Investigation steps:**
1. Split timing: addModule vs lookup vs actual execution
2. Check if lazy compilation (LLJIT with CompileOnDemandLayer) helps

**Potential fixes:**
- Use lazy JIT (`llvm::orc::LLJITBuilder` with lazy compilation) — only compile functions when first called
- Pre-warm the JIT in the constructor instead of per-execution

## Recommended Execution Order

1. **O0/O1 in JIT mode** — easiest win, ~17% savings, one-line change in `optimize()`
2. **Re-run profiling after comment fix** — the 48 `#` warnings in readme_demo were generating report output; now fixed, parse time should drop
3. **Skip unused MLIR dialects** — skip linalg/tensor/bufferization/affine/memref when no kernel defs
4. **Profile with perf** — get precise data before tackling parse allocation or JIT lazy compilation
5. **Lazy JIT** — bigger refactor but could cut jit_execute significantly for programs with many unused functions

## Optimal Prompt

```
I want to optimize JIT performance in my sammine-lang compiler. Read knowledge_base/plans/jit_optimization.md for the full plan and profiling data.

Start with item 1: use O0 (or O1) optimization in JIT mode instead of O2. The change is in Compiler::optimize() in src/compiler/Compiler.cpp — check if compiler_options[JIT] == "true" and use OptimizationLevel::O0 instead of O2. Then re-run --time sparse profiling to measure the improvement.

After that, move to item 3: skip kernel-related MLIR dialect registration (linalg, tensor, bufferization, affine, memref) in Compiler::codegen_mlir() when there are no kernel definitions. The kernel module is only created when kernel defs exist, so the dialects are unnecessary overhead for normal programs.

Run the e2e tests after each change to confirm nothing breaks.
```
