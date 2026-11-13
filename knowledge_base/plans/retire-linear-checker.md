# Plan: retire the AST linear checker

Status: in progress. Steps 1 to 3 done.

`LinearTypeChecker` (`src/typecheck/LinearTypeChecker.cpp`, 791 lines) is
what's left of the AST ownership checking: rules that don't depend on
control flow. MIR checks the flow (`OwnershipAnalysis`); this moves the
rest out and deletes the pass, its stage and the `Use` classification it
walks every expression with.

## What it checks, and what happens without it

Measured by compiling each of the 49 tests that expect one of its messages
with the stage skipped:

| Rule | Tests | Without the AST check |
|---|---|---|
| A `ptr<T>` view only as a parameter's or local's whole type ("Non-owning pointer in …") | 17 | compiles, no error |
| A variant's type argument nothing decided ("Could not infer the type arguments") | 6 | 4 crash in `ModuleGen`; the type checker reports 1 (`report_open_type`, where it needs the type) |
| No write through a view ("Cannot write through non-owning pointer") | 5 | compiles, no error |
| No storing a view through a deref (`*p = &x`) | 1 | compiles, no error |
| No `&` of memory a `'ptr` owns (`&*b`, `&b[0]`) | 2 | compiles, no error |
| No `&` of a value that owns memory | 1 | compiles, no error |
| No owning value bound into a partial application | 3 | 2 compile, 1 a leak report |
| No returning a closure that captured a view, directly or in a literal | 9 | compiles, no error |
| No discarded temporary that owns memory | 3 | MIR's backstop: "Temporary owning memory must not be discarded", without the type |
| No wildcard arm over a variant with an owning payload | 2 | MIR reports the payload as leaked |

## Where each goes

**Type rules, to the type checker.** A view's placement is about types
and declarations: a struct's fields, an enum's payloads, an extern's
signature, and is reported at the declaration, the field that holds the
view. MIR sees a struct's type only where something uses it (verifiers on
its aggregate types would report there), so this stays where
declarations are. An open type argument must be
caught before MIR exists, or `ModuleGen` crashes on it. Both go into
`BiTypeChecker`: placement where it resolves a definition, a signature, a
`let`'s type, a literal's type, a generic instance; open types in a walk
over each function's expressions once it is checked (the type checker
reports some already, `report_open_type`).

**View rules, to a new MIR check.** `ViewCheck` (`sammine-check-views`),
run in the `mir` stage beside the ownership check, reporting through the
same `Reportee` path:

- A `sammine.store` whose place is reached through a view (`deref`,
  `through`, `ptr_elem`, `field`, `elem` down to a `ptr<T, view>`): "Cannot
  write through non-owning pointer". `*p = &x` with `p: ptr<ptr<i32>>` is
  this too, so the lateral-escape rule goes: it can't be written any other
  way (a `'ptr` can't hold a view, by placement).
- A `sammine.addr` of a place reached through an owned pointer, or of a
  place holding a value that owns memory. `sammine.lend`, a view for a
  call, stays allowed.
- A `sammine.closure` binding a value that owns memory.
- A closure that bound a view, reaching a `return`, a store, or a value
  returned: followed through SSA from each `sammine.closure` with a view
  operand, through `let`, aggregates, variants and block arguments. The
  AST rule followed names and literals only; this follows every path.
  Neither follows a closure through a call that returns it.

**Gone with drops** (`plans/drops.md`). A discarded temporary is dropped
(step 12), and a wildcard arm drops the payloads it doesn't name. Until
then MIR's ownership check reports both, in its words.

## Messages

MIR's integers are signless (`arith` takes no other), so a message from
MIR can't spell `u32` or `'ptr<u64>`. The MIR messages name the binding
(`'p'`, `'b.data'`), which the error underlines, and drop the type:
"Cannot bind 'p' into a partial application of 'set_to': the closure can
be called more than once". Two of the moved messages spelled a type.

Not taken, for now: lowering could record a binding's type as written on
its `sammine.let` (an optional attribute, as `written` is on a call), for
a message that needs it; a temporary has no `let`, so it covers named
values only. A sammine integer type carrying its sign would cover all, at
the cost of a cast at every `arith` op.

The 5 tests of the temporary and wildcard rules take MIR's present
wording until drops remove them, so the AST checker goes now.

## Steps

Each a commit, the suite passing after each.

1. Open type arguments: reported by the type checker after each function.
   (Done: `OpenTypeCheck`, run in the type-check stage.)
2. View placement: in the type checker. (Done: `ViewPlacementCheck`.)
3. `ViewCheck`: writes through a view, `&` of owned memory and of owning
   values; the lateral-escape test reports the write. (Done. A partial
   application's view of owned memory is a `sammine.lend` to the closure,
   so `sammine.addr` is only ever a `&`. `&` of an owning value means of an
   owned pointer, as the AST rule had it: `&s` of a struct holding one is
   how `str::compare` is called. Two tests passed only because the error
   snippet echoed their CHECK line: they index a pointer outside an unsafe
   function; now they are unsafe and test the rule.)
4. Closures in `ViewCheck`: binding an owning value, a view-capturing
   closure escaping.
5. The temporary and wildcard tests take MIR's messages.
6. `LinearTypeChecker`, its stage and `Use` go; docs.
