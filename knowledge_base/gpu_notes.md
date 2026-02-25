# GPU Notes — Phase 7 & Related Research

## Descend: A Safe GPU Systems Programming Language
**Authors:** Bastian Köpcke, Sergei Gorlatch, Michel Steuwer — PLDI 2024 ([arXiv](https://arxiv.org/abs/2305.03448))

### Summary
Rust-inspired GPU language that brings memory safety to GPU code via:
1. **Ownership + lifetimes** extended to track CPU and GPU memory across host/device boundaries
2. **Views** — abstractions describing safe parallel access patterns over memory regions; compiled into index computations (Lift/DPIA approach)
3. **Hierarchical scheduling** — grid → blocks → threads encoded in the type system
4. **Compiles to CUDA C** (not LLVM IR or MLIR)
5. **Zero overhead** vs hand-written CUDA on reduction, transpose, scan, matmul benchmarks

### Relevance to sammine-lang Phase 7

| Descend concept | sammine-lang / MLIR equivalent |
|---|---|
| Views (safe parallel access patterns) | `linalg.generic` indexing maps |
| Hierarchical GPU scheduling | MLIR `gpu` dialect + `scf.parallel` |
| Ownership/lifetimes for memory safety | Not yet implemented (manual `alloc`/`free`) |
| Compiles to CUDA text | MLIR → LLVM IR → native; could add `gpu` dialect for GPU targets |

### Key Insights
- **MLIR is better-positioned** than Descend's approach: built-in `gpu` dialect, `linalg` dialect, tensor bufferization give GPU codegen via existing lowering pipelines (`gpu-to-nvvm`, `gpu-to-rocdl`)
- **Views ≈ linalg indexing maps**: Descend's statically-verified access patterns map directly to `linalg.generic`'s affine indexing maps; no need to invent a new type — `tensor<NxMxf64>` + linalg ops encode these patterns
- **Ownership is the big missing piece**: preventing GPU data races requires Rust-style borrow checking in the type system — major undertaking, but Descend shows it's feasible with zero overhead
- **Phase 8 (user MLIR blocks)** could let users write `gpu.launch` kernels directly, sidestepping the need for a full scheduling hierarchy in sammine's type system

## Lift: A Functional Data-Parallel IR for High-Performance GPU Code Generation
**Authors:** Michel Steuwer, Toomas Remmelg, Christophe Dubach — CGO 2017 ([PDF](https://steuwer.info/files/publications/2017/CGO-Lift-IR.pdf))

### Summary
Functional IR where GPU programs are compositions of small data-parallel **patterns**. The compiler uses algebraic **rewrite rules** to transform high-level patterns into low-level GPU code, automatically exploring thousands of valid implementations. Performance on par with hand-optimized OpenCL.

### Core Patterns
- **High-level:** `map`, `reduce`, `zip`, `split`, `join`, `iterate`, `reorder`, `transpose`
- **Low-level (OpenCL-mapped):**
  - `mapGlobal`/`mapWorkgroup`/`mapLocal`/`mapSeq` — map to GPU thread hierarchy
  - `toGlobal`/`toLocal`/`toPrivate` — map to OpenCL address spaces
  - `asVector`/`asScalar`/`Vectorize` — SIMD vectorization
- **Rewrite rules:** algebraic transforms, e.g. `map(f) ∘ map(g) → map(f ∘ g)` (fusion)

### 5-Stage Compilation Pipeline
1. **High-level expression** — user writes `reduce(+, 0) ∘ map(f)` over arrays
2. **Rewrite rules** — transform into low-level OpenCL-mapped patterns (mapGlobal, toLocal, etc.)
3. **Type analysis** — array types carry size info, compiler infers thread counts
4. **Memory allocation + views** — compiler builds **views** (internal data structures describing how to index into memory); array accesses, tiling, padding handled without actual memory movement
5. **Barrier elimination + OpenCL codegen** — barriers inserted only where data dependencies require; emit OpenCL kernel

### Key Insight: Views
Performance-sensitive details (memory layout, indexing, synchronization) are NOT explicit in the IR. The compiler exploits pattern semantics to:
- Infer memory allocation sizes
- Compute multi-dimensional array indices
- Insert barriers only where data dependencies require them
- Eliminate redundant copies

### Performance
- On par with hand-optimized OpenCL
- Compiler can explore **50,000 valid OpenCL kernel variants** from a single expression, each provably correct

### Rise: The MLIR Successor
- [Rise](https://rise-lang.org/) is Lift's spiritual successor, implemented as an **MLIR dialect**
- Lowers functional patterns through `linalg`, `scf`, and `std` dialects
- Rise's existence directly influenced MLIR's `linalg` dialect design
- Compiler: [Shine](https://github.com/rise-lang/shine) — produces C, OpenMP, OpenCL, or CUDA
- MLIR integration: [rise-lang/mlir](https://github.com/rise-lang/mlir-doc)

### Relevance to sammine-lang (most directly compatible paper)

| Lift/Rise concept | sammine-lang connection |
|---|---|
| Functional pattern IR (map, reduce, zip) | Could become built-in higher-order functions |
| Rewrite-rule optimization | MLIR transform dialect / pass infrastructure |
| Views (implicit array indexing) | `linalg.generic` affine indexing maps |
| `mapGlobal`/`mapLocal`/`mapSeq` | `scf.parallel` → `gpu.launch` lowering |
| Rise as MLIR dialect | Could import Rise patterns via Phase 8 plugin system |
| 5-stage pipeline | Stages 1-3 exist; views + barriers come with `linalg` |

### Implications for Roadmap
- **Phase 7:** `linalg.generic` encodes the same framework Lift pioneered; `matmul(a,b)` → `linalg.matmul` is Lift's approach via MLIR. View system = linalg indexing maps.
- **Phase 8:** `--mlir-plugin=path.dylib` could load Rise dialect, enabling Lift-style functional patterns through existing MLIR lowering.
- **Rewrite rules vs direct codegen:** current MLIR backend does direct AST→MLIR. Lift's insight: composable rewrite rules via MLIR transform dialect yield better GPU code than hardcoded lowering. Emit into `linalg` rather than going straight to `scf`/`memref`.
- **No custom dialect needed:** Rise validated that functional patterns lower entirely through existing dialects (`linalg` → `scf` → `memref` → `llvm`).

---

## Phase 7 Design Notes

### Planned dialect usage
- `tensor` dialect for ranked tensor types (`tensor<NxMxf64>`)
- `linalg` dialect for structured ops (`linalg.matmul`, `linalg.generic`)
- `bufferization` to convert tensor → memref before lowering
- New passes: `one-shot-bufferize`, `convert-linalg-to-loops`, `lower-affine`

### GPU extension (future)
- MLIR `gpu` dialect for kernel launch / device memory
- Lowering: `gpu-to-nvvm` (NVIDIA) or `gpu-to-rocdl` (AMD)
- `scf.parallel` → `gpu.launch` transformation available in MLIR

### References
- [Descend paper (arXiv)](https://arxiv.org/abs/2305.03448)
- [Lift IR paper (CGO 2017)](https://steuwer.info/files/publications/2017/CGO-Lift-IR.pdf)
- [Rise language (Lift successor)](https://rise-lang.org/)
- [Rise in MLIR (CC 2021)](https://dl.acm.org/doi/10.1145/3446804.3446844)
- [Lift project docs](https://lift-project.readthedocs.io/en/latest/lift-overview/)
- [MLIR GPU Dialect](https://mlir.llvm.org/docs/Dialects/GPU/)
- [MLIR Linalg Dialect](https://mlir.llvm.org/docs/Dialects/Linalg/)
- [Linalg Dialect Rationale (mentions Lift influence)](https://mlir.llvm.org/docs/Rationale/RationaleLinalgDialect/)
- [GPU Tensor Core codegen with MLIR](https://mlir.llvm.org/OpenMeetings/2021-08-26-High-Performance-GPU-Tensor-CoreCode-Generation-for-Matmul-Using-MLIR.pdf)
