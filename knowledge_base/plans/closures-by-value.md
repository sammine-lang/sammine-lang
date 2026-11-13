# Plan: closures carry what they bind

Status: stage 1 done. Stage 2 not started.

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

## Stage 2: specialization (later)

Lambda set variables in function types, unified by the type checker, with
inclusion constraints; higher-order functions copied per lambda set, as
generics are per type; values become tagged unions and calls switches or
direct calls. The paper's sections 4 and 5. A performance step: stage 1
already gives the memory model.

## Threads

`thread::spawn(f, arg)` copies `f`, with its data, into the new thread's
box and moves `arg` there; `join` frees the box. A closure holding a view
can't be spawned: spawn's parameter escapes.
