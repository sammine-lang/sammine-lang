# Plan: split the ownership check

Status: done.

`src/dialect/OwnershipCheck.cpp` was 1152 lines doing five jobs: paths into
values, the dataflow lattice, the transfer function, the wording of every
message, and the pass. The drops pass (plans/drops.md, steps 6 on) needs the
analysis without the messages, which `Transfer(owners, reports)` couldn't
give it. Behavior stays as it is: the suite guards it, message for message.

## Pieces

| File | Holds |
|---|---|
| `OwnershipPaths.h/.cpp` | `Step`, `Path`, `typeOf`, `partsOf`, `pathOf`, `nameOf`: which part of what an op's value names, and its name in the source |
| `OwnershipAnalysis.h/.cpp` | `Owner`, the lattice and its join, the transfer function (`applyOp`, `applyEdge`), the solver run (`OwnershipFlow`); what it finds wrong goes to an `OwnershipObserver` |
| `OwnershipCheck.cpp` | The observer that reports: each message's words, one report per mistake, loops by `CFGLoopInfo`; the walk and the pass |

The drops pass will be a second observer, or a client of `OwnershipFlow`'s
states: "still owned at a return" is where a drop goes.

## The lattice

An `Owner` was a kind, where it got its value, and where, by what op and
why it was moved: a string. The why is a function of the op that moved it
(`sammine.let` moves, `sammine.free` drops, a call passes, ...), so the
owner keeps the op and the words are found when reporting. "Never held a
value" was "moved, with no reason"; it is "moved by no op".

## Not done

An op interface declaring each op's ownership effect, in place of the
analysis' TypeSwitch over ops: the effects are the analysis' own notions
(paths and steps), and the one dialect it covers is ours. Revisit if a
second dialect needs checking.

## Result

| File | Lines (code) |
|---|---|
| `OwnershipPaths.h/.cpp` | 104 + 198 (176) |
| `OwnershipAnalysis.h/.cpp` | 108 + 629 (502) |
| `OwnershipCheck.cpp` | 321 (259) |

More lines than the one file, for the headers and the observer between
them; the analysis alone is about 500 lines of code, not the 400 hoped
for: the transfer function's cases over ops are most of it.
