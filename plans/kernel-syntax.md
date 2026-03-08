# Plan: Kernel Syntax — Parallel Patterns + Einstein Index Notation

## Goal

Define extensible syntax for expressing parallel computations (matmul, dot product, convolution, etc.) that the compiler can lower to either CPU loops or GPU kernels via MLIR.

## Design Principles

- User writes the **intent**, compiler decides **where** it runs (CPU vs GPU)
- Parallel patterns (`map`, `reduce`) are compiler intrinsics, not stdlib functions
- Einstein-style index notation maps directly to `linalg.generic` indexing maps
- No full dependent types — size-polymorphic types via unification (same as generics)

## Array Syntax Change

Current `[T;N]` composes badly for multidimensional arrays. Change to `[N]T`:

```
// Before (Rust-style, inside-out nesting)
[[f64;4];3]

// After (dimension-major, reads like indexing)
[3][4]f64
```

`[M][K]f64` reads left-to-right matching `a[i,k]` indexing. Internal `ArrayType(element, size)` representation stays the same — only parser and pretty-printer change.

## New Keywords / Intrinsics

| Keyword | Semantics | MLIR lowering |
|---------|-----------|---------------|
| `map` | Parallel element-wise transform | `linalg.generic` (parallel iterators) |
| `reduce` | Collapse a dimension with an op + identity | `linalg.generic` (reduction iterator) |
| `sum` | Sugar for `reduce(k, +, 0.0)` | same |
| `prod` | Sugar for `reduce(k, *, 1.0)` | same |
| `max` | Sugar for `reduce(k, max, -inf)` | same |
| `scan` | Prefix sum (later) | TBD |
| `filter` | Predicated selection (later) | TBD |

These are **not** regular functions — the compiler owns them and can lower to:
- **CPU**: `scf.for` or `linalg.generic` -> loops
- **GPU**: `linalg.generic` -> `scf.parallel` -> `gpu.launch`

## Einstein Index Notation

### Core syntax

```
reduce(bound_indices, op, identity) { body_expr }
```

With sugar:

```
sum(k) { a[i,k] * b[k,j] }
// desugars to:
reduce(k, +, 0.0) { a[i,k] * b[k,j] }
```

### Index variable resolution

In `sum(k) { a[i,k] * b[k,j] }`:

- **Bound indices** (`k` in `sum(k)`) — reduced away, become `"reduction"` iterators
- **Free indices** (`i`, `j` — appear in indexing but not in any reduction) — inferred from operand types, become `"parallel"` iterators and map to output dimensions

### Compilation to linalg.generic

```
fn matmul(a: [M][K]f64, b: [K][N]f64) -> [M][N]f64 =
  sum(k) { a[i,k] * b[k,j] }
```

The compiler extracts affine maps from index expressions:

```mlir
linalg.generic {
  indexing_maps = [
    affine_map<(i,j,k) -> (i,k)>,    // a's indices
    affine_map<(i,j,k) -> (k,j)>,    // b's indices
    affine_map<(i,j,k) -> (i,j)>     // result's indices
  ],
  iterator_types = ["parallel", "parallel", "reduction"]
} ins(%a, %b : ...) outs(%result : ...) {
  ^bb(%a_elem, %b_elem, %acc):
    %prod = arith.mulf %a_elem, %b_elem
    %sum = arith.addf %acc, %prod
    linalg.yield %sum
}
```

### Examples

```
// Dot product
fn dot(a: [N]f64, b: [N]f64) -> f64 =
  sum(i) { a[i] * b[i] }

// Matrix multiply
fn matmul(a: [M][K]f64, b: [K][N]f64) -> [M][N]f64 =
  sum(k) { a[i,k] * b[k,j] }

// Batched matmul
fn bmm(a: [B][M][K]f64, b: [B][K][N]f64) -> [B][M][N]f64 =
  sum(k) { a[batch,i,k] * b[batch,k,j] }

// 1D convolution
fn conv1d(input: [N]f64, kernel: [K]f64) -> [N]f64 =
  sum(k) { input[i+k] * kernel[k] }

// Transpose (no reduction, just index reordering)
fn transpose(a: [M][N]f64) -> [N][M]f64 =
  a[j, i]

// Element-wise (map is an intrinsic)
fn relu(a: [N]f64) -> [N]f64 =
  a |> map(fn(x) => max(x, 0.0))
```

## Size-Polymorphic Types

`M`, `K`, `N` in `[M][K]f64` are **size variables**, unified the same way as generic type variables:

- Concrete sizes (literals): resolved at compile time via monomorphization
- Dynamic sizes (from arguments): checked at runtime with assertions at function boundaries
- No arithmetic on sizes (no `[2*N]f64`) — avoids the need for dependent types or SMT solvers

Start with Futhark's approach: static when possible, runtime assertions otherwise.

## Type Checker Changes

1. Resolve index variables to dimensions by matching against operand types
2. Verify bound indices (`k`) have consistent sizes across operands (e.g., `K` in `a[i,k]` matches `K` in `b[k,j]`)
3. Verify free indices (`i`, `j`) map to the declared return type dimensions
4. This is unification — same algorithm already used for generics/monomorphization

## Implementation Order

**Phase 7a — Kernel linalg codegen (current work, see `plans/kernel-linalg-codegen.md`):**
1. CMake + dialect registration (linalg, tensor, bufferization, affine, memref)
2. Lowering pipeline (add bufferization + linalg-to-loops + memref-to-llvm passes)
3. `convertTypeForKernel()` + `buildKernelFuncType()` (Array → tensor)
4. `emitKernelDef()` rewrite: `linalg.map` for map, `linalg.reduce` for reduce
5. CPU↔kernel call boundary resolution
6. E2E tests with actual kernel computation

**Phase 7b — Einstein index notation (future):**
1. Array syntax change: `[T;N]` -> `[N]T` (parser + printer)
2. Size-polymorphic arrays: `[N]T` where `N` is a size variable
3. `sum(k) { a[i,k] * b[k,j] }` → `linalg.generic` with affine maps
4. `prod`, `max`, `scan`, `filter` sugar

**Phase 7c — GPU lowering (future):**
1. `linalg` → `scf.parallel` → `gpu.launch` (pass pipeline behind `--gpu` flag)
2. `gpu-kernel-outlining` → `gpu.module` + `gpu.func`
3. `convert-gpu-to-nvvm` / `convert-gpu-to-rocdl`

### MLIR lowering strategy
- Kernel code uses **tensor** types (value semantics, no address spaces)
- `linalg.map`/`linalg.reduce` are the highest-level ops — no GPU annotations at this level
- Address spaces appear automatically during `convert-gpu-to-nvvm` (Phase 7c only)
- CPU/GPU boundary is implicit: CPU functions use `!llvm.array`/`!llvm.ptr`, kernel functions use `tensor`/`linalg` — passes only transform ops from their own dialect

## Prerequisites

- MLIR backend Phase 3-6 (pointers, arrays, structs, closures in MLIR) should ideally be done first
- Phase 7a can proceed now since kernel parsing + type checking already work
- `mlir { ... }` escape hatch (Phase 8) provides fallback for unsupported ops

## References

- Einstein summation notation (physics, 1916)
- [Tensor Comprehensions](https://arxiv.org/abs/1802.04730) (Facebook Research, 2018) — direct ancestor of this syntax
- [Lift IR](https://steuwer.info/files/publications/2017/CGO-Lift-IR.pdf) (CGO 2017) — map/reduce as parallel patterns
- [Rise/Shine](https://rise-lang.org/) — Lift's MLIR successor
- [Futhark](https://futhark-lang.org/) — functional GPU language with size-polymorphic types
- [Halide](https://halide-lang.org/) — algorithm/schedule separation
- [MLIR linalg dialect](https://mlir.llvm.org/docs/Dialects/Linalg/) — compilation target
