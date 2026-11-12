# Plan: a mid-level IR (MIR) as an MLIR dialect

Status: design, for review. No code yet.

## Why

The AST does four jobs today: syntax, typed semantics, control flow and the
input to codegen. Every pass rediscovers meaning from the tree's shape:

- Meaning lives in side tables keyed by node id (`CallProps::resolved_name`,
  `BinaryProps::resolved_op_method`, `CastProps::instance`,
  `IndexProps::indexer_method`), and each pass must know which to read.
- Desugaring is spread across passes: `x[i]` becomes an `Indexer` call in
  codegen (`emitIndexerCall`) while the linear checker separately knows it is a
  lend; operators become typeclass calls in codegen (`emitBinaryTypeclassOp`);
  `for` becomes `Iterator<Range>` calls in codegen; implicit returns are added by
  mutating the AST (`GeneralSemanticsVisitor`).
- Rules are written twice: `write_root` in the type checker mirrors the linear
  checker's `Use::Write`, and each pass has its own idea of a place.
- The linear checker rebuilds control flow by hand: snapshot/restore around
  branches, a `diverged_` flag for `return`, a special rule for loops. The bugs
  fixed recently (nested paths, partial moves, a `return` inside an expression,
  returning branches, pointees through `*`) were all this shape.
- A new construct touches ~15 files (token, AST, visitor hooks, clone, type
  checker, linear checker, codegen switch).

Rust (MIR, borrowck on it), Swift (SIL with ownership) and Flang (HLFIR/FIR,
MLIR dialects) all type-check on a tree, then lower to a mid-level IR with
explicit control flow where every name is resolved, and check ownership there.

## Pipeline

```
lex → parse → imports → semantics → typecheck → monomorphize
    → lower to MIR → MIR verify → ownership check (dataflow) → MIR to LLVM dialect
    → existing LLVM pipeline (optimize, emit, link, JIT)
```

- The type checker stays on the AST: its diagnostics are good and it works.
- Only concrete code is lowered: generic templates never reach MIR, their
  monomorphized copies do (as today in codegen).
- Kernels (`kernel` defs → linalg/GPU) keep their current AST path at first;
  see "Out of scope".
- During the migration, a function MIR can't lower yet goes through today's
  AST codegen instead (see "Migration").

## The dialect

The existing `sammine` dialect grows to hold MIR. Today it only has the GPU
transfer ops (`to_device`, `to_host`) in `include/dialect/SammineOps.td`; MIR ops and types are
added beside them.

### Types

Each kind of sammine type gets its own MLIR type, defined in TableGen, so MIR
can be printed and parsed back:

| sammine | MIR |
|---|---|
| `bool`, `char`, `i32`, `i64`, `f32`, `f64` | builtin `i1`, `i8`, `i32`, `i64`, `f32`, `f64` |
| `u32`, `u64` | builtin `i32`, `i64`; signedness is on the ops, as `arith` does (`divsi`/`divui`) |
| `()` | `!sammine.unit` |
| `'ptr<T>` / `ptr<T>` | `!sammine.ptr<T, owned>` / `!sammine.ptr<T, view>` |
| `[N]T` | `!sammine.array<N x T>` |
| `(A, B)` | `!sammine.tuple<A, B>` |
| struct | `!sammine.struct<"vec::Vec<i32>">`, an identified type whose fields are set once, as `LLVMStructType` does, so names stay canonical |
| enum | `!sammine.enum<"Option<i64>">`, identified the same way, with variants and payloads |
| function value | `!sammine.fn<(A) -> R>` |
| a place, a memory location a value lives in | `!sammine.place<T>` |

Linearity is computed from the type (an owned pointer, or anything holding
one), as `Type::containsLinearTypes` does now. A small converter turns MIR types
back into the sammine spelling for diagnostics (`'ptr<i32>`, `vec::Vec<i32>`),
and lowering to LLVM gets its own type converter in place of `convertType`.

### Ops

Places (pure; they name storage, read nothing):

| Op | Meaning |
|---|---|
| `sammine.local "x" : place<T>` | a local's slot, declared once per `let`/parameter, with its source location |
| `sammine.field %p, "f"` | field `f` (or tuple element) of place `%p` |
| `sammine.deref %v` | the place an owned or view pointer value points at |
| `sammine.elem %p, %i` | an array element (bounds-checked when lowered) |
| `sammine.ptr_elem %v, %i` | element `i` behind a pointer: only in `let unsafe` functions, never tracked |
| `sammine.payload %p, variant, k` | payload `k` of an enum place known to be `variant` |

Uses of a place. Each says what it does to ownership, which is what the
checker reads:

| Op | Meaning |
|---|---|
| `sammine.move %p -> T` | takes the value out; the place is empty after |
| `sammine.copy %p -> T` | reads a value that owns nothing (verifier: `T` must not contain linear types) |
| `sammine.lend %p -> ptr<T>` | a view of the place for the length of a call (`ptr<T>` parameters, `x[i]` through `Indexer`, `&x`) |
| `sammine.store %v, %p` | puts a value in a place; the place must not hold a live owner |

Values:

| Op | Meaning |
|---|---|
| `sammine.const` | literals, strings |
| `sammine.call @f(...)`, `sammine.call_indirect` | resolved calls: operators, `Indexer`, `Convert`, `Iterator::next` are calls by this point |
| `sammine.closure @f(bound...)` | partial application, as today's fat pointer |
| `sammine.struct`, `sammine.tuple`, `sammine.enum variant(...)`, `sammine.array` | aggregates, built from moved or copied values |
| `sammine.alloc<T>(n)`, `sammine.free %v` | heap; `alloc` gives an owner whose pointee is empty |
| `sammine.tag %p` | an enum's tag, for `case` |
| arithmetic, comparison, casts | `arith` ops on scalars (or thin `sammine` ones where signedness needs it) |

Control flow: blocks with `cf.br`, `cf.cond_br` and `cf.switch` (on enum tags),
plus `sammine.return %v` and `sammine.unreachable`. `if`, `case`, `while`, `for`,
`&&`/`||` become blocks. There are no structured regions, so the checker sees
one graph.

### Desugaring done while lowering, once

- operators → `Add<T>::Add`, `Less<T>::Less`… calls (built-in instances stay
  `arith` ops)
- `x[i]` on a non-array non-pointer → `sammine.lend` + `Indexer` call
- `x as T` → `Convert` call or `arith` cast
- `for i in a..b` → `Iterator<Range>::next` loop
- implicit returns → `sammine.return`; the `GeneralSemanticsVisitor` rewrite goes
- `case` → `sammine.tag` + `cf.switch` + `sammine.payload`
- tuple destructuring → field moves

### Locations

Every op gets a `FileLineColRange` (start and end line/column; it exists in our
MLIR), mapped back to sammine's byte-offset `Location` through the same line
table `MLIRGenImpl::loc` uses, so the `Reporter` renders spans as precisely as
today. Ownership ops also carry a `reason` string attribute ("passed to
function", "returned", "matched in case") for messages.

## The ownership check on MIR

Built on MLIR's dataflow framework rather than a hand-written walk:

- An `OwnershipAnalysis : dataflow::DenseForwardDataFlowAnalysis<OwnershipState>`,
  where `OwnershipState` is an `AbstractDenseLattice` (the map below, with a
  `join`). Its `visitOperation` is the transfer function for each MIR op:
  `move` marks a path `Moved`, `store` makes it `Live`, `lend`/`copy` only
  check, and so on. The framework iterates to a fixed point, so loops and joins
  need no code of ours.
- It runs in a `DataFlowSolver` with `DeadCodeAnalysis` loaded, which the dense
  analyses need to know which blocks run. The solver is set to
  intraprocedural (`setInterprocedural(false)`): ownership across a call is
  stated by the callee's signature, so each function is checked alone.
- After `solver.initializeAndRun(func)`, one walk reads the state before each
  op and reports errors; reporting stays out of the transfer function, which
  may run many times.
- The ownership check must not depend on constant values (`if true { … }`),
  as Rust's borrow checker doesn't. `loadBaselineAnalyses` also loads constant
  propagation, which would prune branches; whether `DeadCodeAnalysis` alone
  keeps both successors of a `cond_br` live is to be checked first thing when
  the ownership check starts.
- Reference: `mlir/test/lib/Analysis/DataFlow/TestDenseForwardDataFlowAnalysis.cpp`
  in our LLVM checkout.

The state maps place paths (`x`, `x.f`, `*x.p`, …, the same
tree `VarInfo` holds today) to:

- `Live(def)`: holds an owner
- `Moved(where, reason)`: consumed on every path
- `Empty`: never held one (`alloc`'s pointee, a declared but unset slot)
- `MaybeMoved`: consumed on some paths only (the join of `Live` and `Moved`)

Errors:

| Today | On MIR |
|---|---|
| use after consume | a `move`/`copy`/`lend` of a `Moved` place |
| "consumed in some branches but not others" | a `MaybeMoved` place used, or still `MaybeMoved` at a return |
| leak at scope exit / at a return | at each `sammine.return`, any `Live` owner |
| "cannot consume outer var inside a loop" | falls out: the back edge joins `Moved` into the loop head, so the next iteration's use is a use-after-move |
| free/store over a live owner | `store`/`free` checks of the target's pointee |
| partial moves, nested fields, deref paths | path keys, as today's tree |

Branches that return, nested returns and (later) `panic` need no special case:
they are blocks with no edge to the join.

Checks that are not about flow stay where they are or become MIR verifier
rules: where a view `ptr<T>` may appear (`check_placement`), closures capturing
views, lending and moving the same value in one call.

## Migration: one construct at a time, end to end

MIR grows by vertical slices, not layers. Each commit takes one construct all
the way through (lowered to MIR, generated from MIR, tested), and the whole
suite passes after every commit.

### Per-function fallback

- Each function is first lowered to MIR. If it uses a construct MIR doesn't
  handle yet, the lowering gives up on that function and today's AST codegen
  emits it instead.
- Both paths emit into one MLIR module. A function through MIR is defined
  once, with its MIR signature; the AST codegen doesn't forward-declare it.
  Where a call crosses between the paths, the types that differ (pointers,
  structs, enums, tuples, function values; scalars are the same on both sides)
  are bridged with `builtin.unrealized_conversion_cast`, MLIR's no-op op for
  half-lowered code. The module is valid at every point. Once MIR is converted
  to LLVM dialect both sides of each cast have the same type, and
  `reconcile-unrealized-casts`, which the pipeline already runs
  (`MLIRLowering.cpp`), deletes them. For example, with `take` through MIR and
  `main` through the AST:

  ```mlir
  func.func @take(%p: !sammine.ptr<i32, owned>) -> i32 { ... }

  func.func @main() -> i32 {
    %raw = llvm.call @malloc(%size) : (i64) -> !llvm.ptr
    %p = builtin.unrealized_conversion_cast %raw : !llvm.ptr to !sammine.ptr<i32, owned>
    %r = func.call @take(%p) : (!sammine.ptr<i32, owned>) -> i32
    func.return %r : i32
  }
  ```

  After conversion `@take` takes `!llvm.ptr`, the cast is `!llvm.ptr` to
  `!llvm.ptr`, and reconciling removes it. A call the other way casts MIR
  types to LLVM ones, and the ownership check counts that cast as a move.
- The conversion builds signatures and layouts with the same code the AST
  codegen uses (`buildFuncType`, `convertType`, name mangling, `CABI`), so
  both sides agree, until the old path is gone.
- `--mir` turns the MIR path on. Lit runs the whole suite both ways. A debug
  counter prints how many functions went through MIR, so progress shows.

### Slices

Done: (1) `--mir`, the fallback, and functions over `i32`/`i64` with
arithmetic and `return`; (2) `let` of those; (3) the other scalars, `bool`,
`char`, `f32`, `f64`, with their literals and float arithmetic.

Measured after (2), what kept user functions out of MIR was mostly their
types (pointers, views, floats, structs, `Vec`, `Option`, function values in
signatures and `let`s), well ahead of calls (139) and `if` (55). So types
come next, then the constructs, each picked by what blocks the most code:

4. `!sammine.ptr` (owned and view) in signatures and `let`s, with the
   `unrealized_conversion_cast` boundary at calls into and out of MIR;
   `alloc`, `free`, `*p`, lending.
5. Calls between functions, then C calls and variadics (`std::printf`).
6. Structs, tuples, field access.
7. `if`/`else`, comparisons, `&&`/`||`.
8. Enums and `case`, `Option`.
9. Then the rest, one construct per commit: `while`, `for`; operators and
   casts through typeclasses; `Indexer`; closures and partial application;
   arrays and bounds checks; strings; unsafe pointer indexing; instance and
   generic-copy linkage.

Once the fallback is rare and reliable, `--mir` becomes the default; the AST
path still covers what MIR doesn't.

### The ownership check, the same way

- `sammine-opt` (parses MIR, runs passes, prints it) came with the first MIR
  type, `!sammine.ptr`, whose round trip and errors it tests; the check's
  passes are tested through it on small hand-written inputs.
- The MIR check runs only on functions that went through MIR, next to
  `LinearTypeChecker`, which stays in charge. Disagreements on the suite are
  printed (60 test files check linear diagnostics today).
- It covers constructs as MIR does: moves of locals first, then fields and
  partial moves, pointees, `free`, branches and loops.
- When every function goes through MIR and the checks agree, apart from the
  agreed changes (each noted in its commit), the MIR check takes over.

### Cleanup

Delete AST codegen (`MLIRGen*.cpp` except kernels, ~3,400 lines), the flow
parts of `LinearTypeChecker` (~1,400 lines), the side tables MIR replaces, and
the implicit-return rewrite.

### Review checkpoints

The first slice is shown before it is committed; after that, a stop every five
or so commits. Expect 40–60 small commits in all.

Then features waiting on MIR: `panic` (a call + `sammine.unreachable`), `str`
indexing, automatic drops.

## Out of scope, for now

- Kernels keep their AST → linalg path. Moving them to MIR is a separate step.
- Borrowing beyond today's call-length lends.
- Optimizations on MIR.

## Risks

- Diagnostic parity: ~250 error tests compare exact output. The ownership
  check's handover is where most of the care goes.
- Compile time: one more IR and a dataflow pass. Measure with `--time` at each
  step against `readme_demo.mn` (see `jit_optimization.md`).
- Two codegen paths live side by side for most of the migration. Every call
  between them must agree on layout and calling convention; sharing
  `convertType` and `CABI`, and running the suite both ways, keeps them in
  step.

## Decided

1. MIR goes in the existing `sammine` dialect.
2. Each sammine type kind gets a full MLIR type, so MIR is parsable.
3. Consuming an outer value inside a loop is reported as a use after move on
   the next iteration, so consuming and then leaving the loop is accepted.
4. MIR is printed with `--mir --emit mlir` (its functions are marked
   `sammine.mir`, under a `// MIR: N of M functions` line). `sammine-opt`
   parses and prints MIR and runs passes on it, for small hand-written tests;
   it came with the first MIR type.
6. Migration by vertical slices with a per-function fallback (above).
5. The ownership check uses MLIR's dataflow framework (above).

## Open questions

- Where a branch mismatch is reported: where the value is next used (or at the
  return), as the dataflow state gives it, or still at the `if`. To be
  discussed before the ownership check covers branches.
