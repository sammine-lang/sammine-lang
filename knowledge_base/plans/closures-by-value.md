# Plan: closures carry what they bind

Status: stage 1 done; stage 2a (monovariant lambda sets) done; stage 2b
(specialization per lambda set) done. Sets that hold themselves (`μ`)
stay stage 1, by design (below).

## Goal

A closure can go anywhere a value can: returned, stored in a struct, an
array or an enum, put behind an owned pointer, moved to a thread. Nothing
is heap allocated for it, nothing is reference counted, and the program
writes nothing to say a closure escapes.

Today `add(1)` keeps the `1` in the stack frame of the function that makes
it and the closure points there, so a closure that outlives that frame
reads freed stack:

```sammine
let make() -> (i32) -> i32 { add(1) }   // printed 1601658464, not 3
```

## The idea: defunctionalization

A function value is the function it calls and what it binds, by value: a
closure is a small struct, copied like one. This is defunctionalization
(Reynolds 1972; MLton): each function type stands for the set of functions
that can inhabit it (its lambda set), and a value of it holds one of them
with that one's captured data.

Brandon, Driscoll, Dai, Berkow and Milano, "Better Defunctionalization
through Lambda Set Specialization" (PLDI 2023, doi 10.1145/3591260) adds
specialization on top: each higher-order function is copied per lambda set
it is called with, so calls become direct. That is for speed; the memory
model comes from the representation alone. Roc compiles closures this way,
boxing only recursive ones.

## Stage 1: one lambda set per function type

A function type's lambda set is every closure of that type the compilation
unit makes. A value is

```
{ code: ptr, data: [k x i64] }
```

where `k` words hold the largest set of bound values any closure of that
type binds. The code pointer says which function it is, as a tag would;
calls stay indirect, through the code, which is passed a pointer to the
data. A named function binds nothing; a type no partial application makes
has `k = 0` and is a code pointer.

- **Sizes from MIR.** `!sammine.fn<sig, k>` carries `k`, and the AST is
  lowered with `k = 0` everywhere. `sizeClosures`
  (`src/dialect/SizeClosures.cpp`), the last step of making the module,
  reads every `sammine.closure` in it, with the types it binds (the
  callee's parameters, so a lent owned pointer is a view), gives each
  function type its words, and rewrites every type holding one: an enum's
  payload size, part of its type, and the offsets its `variant`s and
  `payload`s carry follow. The closures that size a type are the ones
  lowered, so the two can't disagree; lowering to LLVM only refuses a
  closure held in its maker's frame whose type has no word for the pointer.
- **C sees no function value inside a value.** A C binding declares its
  LLVM types before any body is lowered, so before sizes are known, and a
  struct holding a function value has no layout C could be written
  against anyway: taking or returning one by value is a type error
  (`check_c_signature`). A function parameter is a pointer to its code,
  and a pointer to such a struct is a pointer.
- **What a closure binds can't own memory.** The type checker already
  rejects binding an owned value ("the closure can be called more than
  once"), so copying a closure's data never duplicates an owner, and there
  is nothing to drop.
- **A closure binding a closure of its own type** (directly or through
  others) can't carry it: its size would be infinite. The paper's `μ`
  lambda sets; at run time they need a box. Instead it keeps what it binds
  in its maker's frame, as every closure did before, and its words hold a
  pointer there; it then stays in the call that made it, as a view. Such a
  type is sized as if that closure bound one word. `twice(add(1))` and
  `apply2(add)` are this case; stage 2 knows the bound value is exactly
  `add`, which carries nothing, and lifts it.
- **Calls pass the words by value.** Passing a pointer to a caller's slot
  kept LLVM from turning a self tail call, like `fold`'s, into a jump: it
  ran out of stack at a few hundred thousand steps.
- **Units agree on sizes.** Two units can size one type differently, so a
  closure must not cross from one unit's code to another's. A function
  whose signature has a function type, from another module, is compiled in
  the module that uses it, as a generic function is, with internal linkage.
- **Views.** A closure binding a view (`look(b)`, b lent) holds that view:
  it may be called and passed down, but must not outlive the call that
  made it, the rule for `ptr<T>`. A MIR analysis follows each function
  value from where it is made: returned, stored, put in an aggregate, or
  passed where it may escape, a closure holding a view is an error. Each
  function's summary says which function parameters may escape it, to a
  fixed point over the call graph; a call through a function value is
  assumed to let its arguments escape.

## Stage 2a: lambda sets (done)

`src/dialect/LambdaSets.cpp`. One lambda set per flow of function values
across the module, as MLton's defunctionalization has them: unification
along every op that moves a value, each `closure` in its result's set. A
value of a known set becomes what its closure binds, a tuple for one
closure, an enum (`lambdas<N>`, a variant per closure) for several; a
`call_indirect` through it a `call` of the closure's function, given what
it binds then the arguments, or a `cf.switch` on the tag of such calls. A
call no closure reaches is `unreachable`, and the blocks after it go.

What it gave:

- **Speed.** Calls through closures are direct, and LLVM inlines them:
  `functions/closure_self_type_returns.mn` folds to constants.
- **Self-binding closures escape.** `twice(add(1))` has set
  `{twice[{add}]}`, finite. Only a set that holds itself (`nest`,
  `error_reporting/closure_holds_itself_escapes.mn`) stays stage 1, and its
  closures that bind themselves stay in their maker's frame.

`markLambdaSets` runs before the checks and marks each closure of a set
that becomes what it binds (`sammine.by_value`): the escape check only
keeps in their frame the stage-1 closures that `bindsInline` says can't
carry what they bind. In the lowering to LLVM, `expandDrops` runs first,
so a Drop instance is called as any function is and its parameter is in
the dropped value's set; then specialization, then `lowerLambdaSets`.
A struct or enum holding a lowered value keeps its name: MIR tells the two
apart by their fields, and `sammine-mir-to-llvm` names the LLVM struct of
each body after the first `<name>.<n>` (`identifiedStruct`). An enum's
payload size and the offsets of its `variant`s and `payload`s are
recomputed. A value of no size, a closure binding nothing,
is neither written to nor read from a payload.

## Stage 2b: specialization (done)

`src/dialect/SpecializeLambdaSets.cpp`, run just before stage 2a's
rewrite. A higher-order function is copied per lambda set it is used
with, as generics are per type: `apply(add(1))` and `apply(mul(2))` call
two copies, each calling its one closure's function directly.

- **Copy per place, callers first.** The unit copied is an SCC of the
  call graph (calls and closures), the functions calling each other: a
  copy of it calls its own copies, so their sets are the copy's (the
  paper's 6.3). Most are one function, calling itself in its own copy.
  Callers before callees, each place naming a function of a private SCC
  of which one takes or gives function values gets its own copy of the
  SCC; the first keeps it. A copy's own places are copied in turn when
  the callee's turn comes. Within a copy the sets are one, as in the
  paper: a member calling back in with a closure of its own adds it to
  the copy's set.
- **Merge per set.** When a function's turn comes, all its callers are
  done: its copies whose signatures are described alike
  (`describeSignatures`: per function value, the functions its closures
  call and what those bind) are merged before their places are copied.
  Functions whose turn hasn't come are opaque to that description: their
  places all name one function still, which would mix every place's sets.
  Without this, copies grew with the paths to a function, 2^n for n
  levels each calling the next twice
  (`functions/closure_specialized_paths.mn`). Once all are taken, copies
  are merged again with every call followed, until none are alike.
- **Alike sets are one type.** A set's enum is named by its description
  and its variants are ordered by it, and two closures calling one
  function with alike bound values are one variant: copies merged from
  places in different callers pass the same type, and no switch picks
  between closures that do the same.
- **Not copied:** a public function (another unit calls it with values as
  they are), a function named other than by a call or a closure, and an
  SCC holding any of them. A Drop instance is copied like any function:
  drops are calls of it by then.
- **Bounded.** Copies may add at most `kCopyBudget` ops; past it, places
  share a function, as without this step.

### Why not in the type checker

The paper infers lambda sets with Hindley–Milner: a set variable rides on
each function type, and let-generalization of a function's scheme, per
SCC, is what makes them polymorphic; instantiating it per call is what
specializes. sammine's checker is bidirectional, and by MIR every type is
known and every generic is monomorphized, so only the sets are left to
infer. That is the same unification, done on MIR after typing, and the
polymorphism comes from copying per place and merging per set rather than
from generalized schemes.

### Against the paper

Section numbers are the paper's (`3591260.pdf`).

- **Same.** Lambda sets are found by unification (4.2.1); the unit of
  specialization is a top-level function, the paper's `def`; sammine has
  no lambdas, only partial applications of named functions, which are
  exactly its `λ[captures](args)` with explicit captures. Specialization
  goes from callers to callees, one copy per distinct set among the uses
  already specialized (5.1, Fig. 9's inner loop). Lowering makes a
  function type a sum of what each closure captures, a closure an
  injection and a call a match (5.2), with one closure a tuple and a
  direct call. Set descriptions are shared by number, not nested, so they
  stay linear (6.1).
- **Different: how instances are found.** The paper infers each `def`
  once, generalizing the set variables of its signature with inclusion
  constraints and internalizing the rest (4.2.2), then instantiates the
  scheme per use. sammine copies per place and unifies over the whole
  program, describing a function's copies with the functions not yet
  specialized left opaque. That is an approximation: a later function
  that unifies a parameter with a closure of its own adds to its caller's
  set, which the opaque description misses, so two copies may merge that
  the paper keeps apart. It stays sound; the merged copy switches.
- **Different, by design: recursive sets.** The paper solves a set that
  holds itself as `μa.σ` (4.2.2, 5.1.1) and lowers it to a recursive sum,
  which has no size without a box on the heap. sammine keeps such a set
  stage 1, `{code, words}`, its self-binding closures in their maker's
  frame, and a closure of one can't leave that call
  (`error_reporting/closure_holds_itself_escapes.mn`). Boxing it would make
  a closure own memory: dropped, and moved or counted rather than copied.
  The language keeps closures to what costs what it looks like, nothing
  allocated or counted behind them, so this stays.
- **Same: SCCs of several functions** (6.3) are copied whole.
- **Not needed: delayed substitution** (6.2): nothing is substituted;
  copies are re-analysed, once per SCC with copies.

## Threads

`thread::spawn(f, arg)` copies `f`, with its data, into the new thread's
box and moves `arg` there; `join` frees the box. A closure holding a view
can't be spawned: spawn's parameter escapes.
