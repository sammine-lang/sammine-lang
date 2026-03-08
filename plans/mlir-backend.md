# Plan: Replace CgVisitor with MLIR Backend

## Status

| Phase | Status | Description |
|-------|--------|-------------|
| 1 | **DONE** | Infrastructure + scalar functions (MVP) |
| 2 | **DONE** | Control flow + variables |
| 3 | TODO | Pointers + arrays |
| 4 | TODO | Structs + strings |
| 5 | TODO | Closures + partial application |
| 6 | TODO | Module system + exports |
| 7 | TODO | Tensor types + linalg (the payoff) |
| 8 | TODO | User MLIR blocks + plugins |

## Pipeline

```
Current:   AST → CgVisitor → LLVM IR → .o
New:       AST → MLIRGen → MLIR (arith/func/scf/memref/llvm) → lowering → LLVM IR → .o
```

Both backends coexist via `--backend=mlir` (default `llvm`). Delete CgVisitor after full parity.

## Dialect mapping

| Dialect | Covers |
|---------|--------|
| `arith` | Arithmetic: add, sub, mul, div, mod, cmp, constants, negation |
| `func` | Function defs, calls, return |
| `scf` | if/else (scf.if), array comparison loops (scf.for) |
| `memref` | Variables (alloca + load/store), arrays, bounds checks |
| `llvm` | Closures, structs, strings (globals), pointers, malloc/free, IFunc, extern |

No custom sammine dialect needed.

---

## Phase 1 (DONE): Infrastructure + Scalar Functions

### Files created/modified
| File | Change |
|------|--------|
| `CMakeLists.txt` | Added `find_package(MLIR)` with brew fallback |
| `src/codegen/CMakeLists.txt` | Added `MLIRBackend` object library + MLIR link targets |
| `src/compiler/CMakeLists.txt` | Added MLIR include dirs and link targets |
| `src/CMakeLists.txt` | Added MLIR include dirs for sammine executable |
| `include/compiler/Compiler.h` | Added `BACKEND` to `compiler_option_enum` |
| `src/sammine.cpp` | Added `--backend` argparse flag (llvm\|mlir) |
| **NEW** `include/codegen/MLIRGen.h` | `mlirGen()` function declaration |
| **NEW** `src/codegen/MLIRGen.cpp` | `MLIRGenImpl` class — AST → MLIR emission |
| **NEW** `include/codegen/MLIRLowering.h` | `lowerMLIRToLLVMIR()` declaration |
| **NEW** `src/codegen/MLIRLowering.cpp` | MLIR → LLVM IR lowering pipeline |
| `src/compiler/Compiler.cpp` | Added `codegen_mlir()` method, dispatched from `codegen()` |
| `include/codegen/LLVMRes.h` | Fixed deprecated `lookupTarget(string)` → `lookupTarget(Triple)` |

### Design decisions
- **NOT using visitor pattern** — direct recursive dispatch (`emitExpr` → `dynamic_cast`) matches MLIR's value-returning emission model
- **Uses existing `LexicalStack`** for scoped variable tracking (avoids `ScopedHashTable<StringRef>` lifetime issues)
- **SSA-only variables** in Phase 1 — no `memref.alloca` yet
- **`proto->type` gotcha**: prototype's `type` field is the full `FunctionType`, not just the return type — must extract via `std::get<FunctionType>(...).get_return_type()`

### Lowering pipeline
```
mlir::PassManager:
  1. createArithToLLVMConversionPass()
  2. createConvertFuncToLLVMPass()
  3. createReconcileUnrealizedCastsPass()
Then: mlir::translateModuleToLLVMIR(module, llvmCtx)
```

### CMake configuration
```bash
cmake -B build -DSAMMINE_TEST=ON \
  -DLLVM_DIR=/Users/jjasmine/Developer/igalia/llvm-project/build/lib/cmake/llvm \
  -DMLIR_DIR=/Users/jjasmine/Developer/igalia/llvm-project/build/lib/cmake/mlir
```

### Verified working
```bash
# All 119 existing tests pass on default backend
cmake --build build -j --target e2e-tests  # 119/119 passed

# MLIR backend compiles and runs scalar arithmetic
./build/bin/sammine -f scratch/test_mlir.mn --backend=mlir
./test_mlir.exe  # exit code 7 (add(3,4))
```

---

## Phase 2 (DONE): Control flow + variables

### What was implemented
- `VarDefAST` (mutable) → `memref.alloca` + `memref.store`; immutable stays SSA
- `VariableExprAST` — checks if stored value is `MemRefType` → `memref.load`, else direct SSA
- `IfExprAST` → `scf.if %cond -> type { scf.yield %then } else { scf.yield %else }`
- Assignment (`BinaryExprAST` with `=`) → `memref.store` to LHS variable's memref
- Unit-typed if/else emits `scf.if` without result types

### New MLIR libraries added
`MLIRSCFDialect`, `MLIRSCFToControlFlow`, `MLIRControlFlowToLLVM`, `MLIRMemRefDialect`, `MLIRMemRefToLLVM`

### Lowering pipeline (updated)
```
mlir::PassManager:
  1. createSCFToControlFlowPass()
  2. createArithToLLVMConversionPass()
  3. createFinalizeMemRefToLLVMConversionPass()
  4. createConvertControlFlowToLLVMPass()
  5. createConvertFuncToLLVMPass()
  6. createReconcileUnrealizedCastsPass()
```

### Design decisions
- **Mutable vs immutable**: distinguished by the MLIR type of the stored value — `MemRefType` means mutable (load on read), otherwise direct SSA
- **No separate tracking structure needed** — LexicalStack stores `mlir::Value` for both, type check disambiguates
- **`scf.if` for control flow** — chosen over `cf.cond_br` for structured semantics and future optimization potential (Phase 7 tensor work)

### Known limitations
- `func.return` inside `scf.if` regions is not valid MLIR — early returns inside if-branches will cause verification errors (to be addressed in a future phase)

### Verified working
```bash
# All 119 existing tests pass on default backend
cmake --build build -j --target e2e-tests  # 119/119 passed

# MLIR backend handles mutable vars, assignment, if/else
./build/bin/sammine -f scratch/test_mlir_phase2.mn --backend=mlir
./test_mlir_phase2.exe  # exit code 17 (test_combined(7) = 7+10)
```

---

## Phase 3: Pointers + arrays
- `ptr<T>` → `!llvm.ptr`
- `DerefExprAST` → `llvm.load`
- `AddrOfExprAST` → direct alloca value
- `AllocExprAST` → `llvm.call @malloc`
- `FreeExprAST` → `llvm.call @free`
- `ArrayLiteralExprAST` → `memref.alloc + memref.store` per element
- `IndexExprAST` → `memref.load %arr[%idx]` with bounds check via `scf.if`
- `LenExprAST` → `arith.constant` (compile-time)

---

## Phase 4: Structs + strings
- `StructDefAST` → `!llvm.struct<(field_types...)>`
- `StructLiteralExprAST` → `llvm.mlir.undef + llvm.insertvalue` chain
- `FieldAccessExprAST` → `llvm.extractvalue`
- `StringExprAST` → `llvm.mlir.global` + `llvm.mlir.addressof`

---

## Phase 5: Closures + partial application
- Closure type → `!llvm.struct<(ptr, ptr)>` (code_ptr, env_ptr)
- `buildClosure` → `llvm.insertvalue` chain
- Wrapper functions → `func.func` with `llvm.call` to original
- Partial application → env struct + wrapper function
- Indirect call → `llvm.extractvalue` code/env + `llvm.call`

---

## Phase 6: Module system + exports
- Function mangling: `module$func` naming in `func.func`
- Extern → `func.func` with external linkage attribute
- IFunc → `llvm.ifunc` + resolver function
- Forward declarations → declare all `func.func` before defining bodies

---

## Phase 7: Tensor types + linalg (the payoff)

**See `plans/kernel-linalg-codegen.md` for detailed implementation plan.**

### Summary
- Kernel functions use `tensor` types (not `!llvm.array`) for array params/returns
- `map(a, lambda)` → `linalg.map` on tensors
- `reduce(a, op, identity)` → `linalg.reduce` on tensors
- Bufferization pass converts tensor → memref before lowering
- CPU path: `linalg` → `scf.for` loops → LLVM
- GPU path (future): `linalg` → `scf.parallel` → `gpu.launch` → nvvm

### New methods
- `convertTypeForKernel()` — Array → `RankedTensorType`, rejects ptr/string/closure
- `buildKernelFuncType()` — uses tensor types, no sret transform

### New MLIR libraries
`MLIRLinalgDialect`, `MLIRLinalgTransforms`, `MLIRTensorDialect`,
`MLIRBufferizationDialect`, `MLIRBufferizationTransforms`, `MLIRBufferizationPipelines`,
`MLIRAffineDialect`, `MLIRMemRefDialect`, `MLIRMemRefTransforms`,
`MLIRMemRefToLLVM`, `MLIRAffineToStandard`

### Lowering pipeline (updated)
```
mlir::PassManager:
  // NEW: tensor/linalg lowering
  1.  one-shot-bufferize{bufferize-function-boundaries}
  2.  buffer-deallocation-pipeline
  3.  convert-linalg-to-loops (nested under func::FuncOp)
  4.  expand-strided-metadata
  5.  lower-affine
  // EXISTING: dialect-to-LLVM
  6.  createSCFToControlFlowPass()
  7.  createArithToLLVMConversionPass()
  8.  createConvertControlFlowToLLVMPass()
  9.  createFinalizeMemRefToLLVMConversionPass()
  10. createConvertFuncToLLVMPass()
  11. createReconcileUnrealizedCastsPass()
```

### Call boundary: auto-generated wrapper (B2)
Each kernel emits two functions: a private `__kernel_<name>` with tensor types (preserves fusion), and a public `<name>` wrapper with `!llvm.ptr` ABI (matches CPU calling convention). This is the CUDA `__global__` stub pattern — codegen generates both, CPU callers only see the wrapper. Memref-directly (B3) was rejected because it kills `linalg-fuse-elementwise-ops` (tensor-only pass). See `plans/kernel-linalg-codegen.md` §Call Boundary for details.

---

## Phase 8: User MLIR blocks + plugins
- `mlir name { ... }` syntax (raw MLIR text, parsed by `mlir::parseSourceString`)
- `mlir name = "path.bc"` (precompiled bitcode)
- `--mlir-plugin=path.dylib` flag (load via `mlir::DialectPlugin::load`)
- Module namespace: `name::func()` via existing qualified name system
