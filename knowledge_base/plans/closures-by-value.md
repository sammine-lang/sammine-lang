# Plan: closures carry what they bind

Status: stage 1 done. Stage 2 is a TODO (below).

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

## Stage 2: specialization (TODO)

Not started. Two things it would give:

- **Speed.** Every call through a function value is indirect, through its
  code pointer, so LLVM can't inline `f` into `fold` or `twice`. With
  lambda sets a call is a direct call when one function can reach it, a
  switch of direct calls when a few can, and each higher-order function is
  copied per lambda set it is called with, so most calls are direct.
- **Self-binding closures escape.** `twice(add(1))` can't be returned
  today (`error_reporting/closure_self_type_escapes.mn`): one size per
  function type can't hold a value of itself. Its lambda set,
  `{twice[{add[i32]}]}`, holds `add`'s, a different and finite set, so
  only truly recursive closures (the paper's `μ` sets) would stay in their
  maker's frame.

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
