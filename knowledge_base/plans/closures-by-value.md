# Plan: closures carry what they bind

Status: stage 1 done; stage 2a (monovariant lambda sets) done; stage 2b
(per-call-site specialization) is a TODO (below).

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

- **Sizes before MIR.** An enum's payload size is part of its MIR type, so
  a closure's size must be known before any MIR type is made. A pass over
  the type-checked AST finds every partial application, with the types it
  binds (the callee's parameters, so a lent owned pointer is a view), and
  `!sammine.fn<sig, k>` carries `k`.
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
carry what they bind. `lowerLambdaSets` runs first in the lowering to LLVM.
A struct or enum holding a lowered value is renamed `<name>#<n>` (its LLVM
struct is identified by name; `dropFunction` looks up the part before `#`),
and an enum's payload size and the offsets of its `variant`s and
`payload`s are recomputed. A value of no size, a closure binding nothing,
is neither written to nor read from a payload.

## Stage 2b: specialization (TODO)

The paper's own step: sets generalized per function (per call-graph SCC,
section 6.3) with inclusion constraints, and each function copied per set
it is called with, as generics are per type. Today a higher-order function
called with `add` in one place and `mul` in another switches between them
inside one copy; specialized, each copy calls one directly.

How, worked out against the code (2026-11):

- **On MIR, not the type checker.** A partial application is the paper's
  lambda with explicit captures, `λ[bound](rest). f(bound, rest)`, and MIR
  has only `cf` branches, so values move between blocks only through block
  arguments. Annotate each MIR value's type with a lambda set variable per
  `!sammine.fn` in it (pre-order over pointees, fields, elements, variants
  and signatures), unify along every op that moves a value (`let`,
  `copy`/`move`/`store` through places, `field`/`elem`/`through`,
  `aggregate`/`extract`, `variant`/`payload`, block arguments, `call`,
  `return`, `call_indirect`), and add each `closure` to its result's set.
- **Two steps.** First monovariant (one set per flow class across the
  module, MLton's defunctionalization): values become a tuple of the
  captures (one function) or an enum of them (several), `call_indirect` a
  `call` or a `cf.switch` of calls. Then the paper's polymorphism: sets
  generalized per function (per call-graph SCC, section 6.3) with
  inclusion constraints, functions cloned per instantiation, as generics.
- **What stays stage 1.** A class reaching code MIR doesn't see (C, a
  library's exported function, another module's) or a recursive (`μ`)
  class keeps the `{code, words}` representation, and so does every class
  its closures capture, so stage 1's sizes still hold for them.
- **Types to rebuild.** A struct or enum holding a converted function value
  gets a new name (its LLVM struct is identified by name), an enum's
  payload size and `variant`/`payload` offsets are recomputed, and
  function signatures change with their parameters.
- **Order.** Inference before `checkViews` (which closures are frame-held
  decides the escape check), the rewrite after `checkOwnership`, before
  `sammine-mir-to-llvm`.

## Threads

`thread::spawn(f, arg)` copies `f`, with its data, into the new thread's
box and moves `arg` there; `join` frees the box. A closure holding a view
can't be spawned: spawn's parameter escapes.
