# Plan: automatic drops

Status: in progress. Steps 1 to 10 are done, and 11 for inserted drops.

## Goal

A value that owns memory and is still owned where it goes out of use is
dropped by the compiler, instead of reported as a leak ("must be consumed
before scope exit"). Every type that owns memory drops. Values that own
memory stay checked for everything else: a use after a move, a part moved
twice, a view outliving its owner.

Such values go from linear (used exactly once) to affine (used at most
once; the compiler handles the rest). A type doesn't opt out to stay
linear: what being linear asked of the programmer, handle the value in the
end, a drop now does. A type whose end must do something particular says
so with a `Drop` instance.

The model is Rust's and C++'s: a type's own drop code runs on a view of
the value, then the compiler drops its fields (Rust's drop glue, C++'s
member destructors).

## Owning types

Two kinds of owned pointer, one per use:

| Type | Holds | Its drop |
|---|---|---|
| `'ptr<T>` | exactly one `T` (a box) | drop `*p`, then free |
| `'buf<T>` (new) | as many `T` as unsafe code says | free only |

`'ptr<T>` is a box: once it leaves the function that allocated it, it
holds a value (see "The box rule"). The check follows `*p`, and a drop
drops it. Indexing a `'ptr<T>` goes, in the last step: a `'ptr<T>` has
one element.

`'buf<T>` is a buffer, as Rust's `RawVec<T>` or C++'s `T*` in
`std::vector`: `alloc_buf<T>(n)` makes one of `n` values; `b[i]` reads and
writes them in unsafe functions only, as `p[i]` does today, and the check
doesn't follow them. Its drop frees the memory and never touches what's in
it: the type that holds the buffer knows how many values are live, and
drops them in its `Drop` instance. A buffer of plain values (`'buf<char>`)
needs none.

Today's uses move over: `Vec<T>`'s `data`, `String`'s `data`, `io`'s read
buffer and `str`'s `malloc` become `'buf`. Of 250 `alloc` calls in `.mn`
files, 229 allocate one value and stay `alloc`.

`io`'s file handle, a `'ptr<char>` holding C's `FILE *`, is neither: its
drop must call `close`, not `free`. Before pointers drop on their own
(step 6), it becomes a struct, `io::File`, which nothing drops until it
has a `Drop` instance that closes it (step 8): until then a `File` not
closed is still an error.

## What dropping a value does

| Type | Drop |
|---|---|
| `'ptr<T>` | drop `*p` if it holds a value, then free `p` |
| `'buf<T>` | free `b` |
| struct, tuple | drop each field that owns memory, in declaration order |
| enum | drop the payload of the variant its tag holds |
| `[N]T` | drop each element, first to last |
| a type with a `Drop` instance | call it, then drop its fields as above |

`Drop<T> { Drop(x: ptr<T>) -> unit; }` is a prelude typeclass. It takes a
view, so the instance doesn't own `x`: nothing is left for it to drop when
it ends, and it can't drop itself again. It can't move a field out of `*x`
either (nothing moves out through a view), so the fields are all there for
the drop that follows. Nothing but a drop calls it; a call of `Drop`
written in code is an error, as in Rust (it would drop the value twice).

`Vec<T>`, with its buffer:

```sammine
export struct Vec<T> { data: 'buf<T>, length: i64, cap: i64 };

export instance<T> Drop<Vec<T>> {
  let unsafe Drop(v: ptr<Vec<T>>) -> unit {
    for i in 0i64..(*v).length { drop((*v).data[i]); };
  }
}
// then v's fields drop: data frees the buffer; length and cap own nothing.
```

## `drop(x)` replaces `free(p)`

With drops, freeing is what dropping a pointer does, and more. So `free`
becomes `drop(x)`, for a value of any type that owns memory: it consumes
`x` and drops it there, instead of where it goes out of use. Afterwards `x`
is moved, as after any move, and no drop is inserted for it. Once `drop`
takes any type (step 9), `drop` of a value that owns nothing does nothing,
as Rust's: generic code drops a `T` that may be `i32`, as `Vec<T>`'s
instance does. Until then it takes pointers only, as `free` did.
The keyword, the token and `FreeExprAST` are renamed, and MIR's
`sammine.free` is `sammine.drop`, the one op that ends a value, written
or inserted. 269 calls in 164 `.mn` files change.

`Vec`'s `grow_buf` copies `old_d[0..l]` to a new buffer and drops `old_d`:
dropping a `'buf` frees it without touching the values copied out, so no
separate "free without dropping" is needed.

## Drop order

Order shows only through a `Drop` instance with effects: printing,
flushing, closing. A view can't make it matter: a view is a parameter or a
local only (`check_placement` keeps one out of a struct, an enum and a
return), has no drop, and can't outlive its owner.

So the choice is the one a reader expects: bindings in reverse order of
binding, fields and elements in declaration order, as Rust. Reverse of last
use was the alternative; it changes as code is edited far from the
binding, so it isn't taken.

## The box rule

The ownership check (`OwnershipCheck.cpp`) follows `*p` as a part when `T`
owns memory, but takes an owned pointer it doesn't see made, a parameter
or a call's result, to point at nothing. That is unsound today, and drops
would inherit it:

```sammine
let boxed() -> 'ptr<'ptr<i32>> {
  let pp = alloc<'ptr<i32>>(1);
  *pp = alloc<i32>(1);
  pp
}
let main() -> i32 {
  let pp = boxed();
  free(pp);         // accepted: the inner pointer leaks
  let q = *pp;      // (instead) rejected: "no owned value is known to be stored there"
  0
}
```

The rule, as Rust's `Box<T>`: a `'ptr<T>` holds a value once it leaves the
function that allocated it. `alloc` gives a pointer to nothing; storing in
`*p` fills it; passing, returning or storing a pointer whose pointee holds
nothing yet is an error ("'p' points at nothing yet"). Everywhere else,
`*p` is live when `T` owns memory: the check's entry state and a call's
result change from "holds nothing" to live. Moving `*p` out leaves `p`
pointing at nothing, so `p` can't be passed on until refilled.

The rule has no exceptions: what needs memory with no value in it yet, or
with many, is a `'buf`.

## Where drops go

A MIR pass, `sammine-elaborate-drops`, inserts `sammine.drop` for each
value still owned where it goes out of use, from the ownership analysis'
states, and marks each it inserts: `sammine.drop %p implicit`. The
ownership check runs after it, on what will run: a leak it still finds is
a type that doesn't drop yet, or a bug in the elaboration. (Rust
elaborates drops after borrow checking; Swift's SIL has its cleanups from
the start and diagnoses after. This is Swift's order.)

The mark keeps the check's messages about the program as written. Where
paths join, one moved by the program and one by an implicit drop, the
value is moved on both; the owner keeps the program's move, which a
message points at, and that some path only dropped it, so a use after it
reads "moved in some branches" (or "in an earlier turn of the loop"), as
before, not "already dropped" at a drop the program doesn't show.

Two rules place every drop but the ones at stores:

- At each `return`: every live part, by binding, in reverse order of
  binding. A partly moved value drops only the parts still live, which the
  analysis knows exactly.
- On each edge into a block: a part live at the end of the edge but not
  live where the block starts, because another edge moved it, or because
  the block is past where its value is defined (a `let` in an arm, at the
  join; a loop body's, at the back edge to the header). It is dropped on
  the edge, before a branch with one successor or in a block of its own.
  The analysis knows every edge's state, so no runtime drop flags are
  needed. This covers joins, the end of a loop's turn and the end of an
  arm, and replaces "consumed in some branches but not others".
- Before a store into a place that still owns something, and before `let`
  shadows, the old value is dropped. ("Reassigning linear variable … without
  consuming its previous value" goes.)
- A temporary that owns memory and isn't taken, `f(alloc<i32>(1) == p)`, is
  dropped at the end of its statement. ("Temporary owning memory must not
  be discarded", the ownership check's since the AST checker went, goes.)
- Not before `exit`, a `panic` or `sammine.unreachable`: the process ends.

A value whose type has a `Drop` instance moves and drops whole, but its
fields are still followed: moving `v.data` out of a `Vec` would leave its
instance without one, so moving a field out of such a value is an error, as
in Rust (E0509).

Inside a loop, consuming a value from outside it stays an error: the next
turn would use it. Taking it out on purpose is spelled by moving it into
something that lives outside the loop.

## Lowering

`sammine.drop %v : T` names a value or place to drop. `'buf<T>` is a third
kind of `!sammine.ptr` (`owned`, `view`, `buffer`), an `!llvm.ptr` after
lowering. Lowering to LLVM, each type gets a drop function generated once
per module, `__drop_<type>`: it calls the type's `Drop` instance if it has
one, with a view of the value, then drops the parts as the table says and
frees an owned pointer or a buffer. An enum's switches on its tag, unless
the analysis knows the variant (inside a case arm), and then drops that
variant's payload inline.

## Diagnostics that change

| Today | With drops |
|---|---|
| "must be consumed before scope exit" | dropped; no error |
| "consumed in some branches but not others" | dropped on the other edges; no error |
| "Reassigning … without consuming its previous value" | old value dropped |
| "Temporary owning memory must not be discarded" | dropped at statement end |
| "Cannot consume … inside a loop" | stays |
| "Cannot use … already moved/freed" | stays, "moved/dropped" |
| "no owned value is known to be stored there" | narrows to `alloc`'s pointer before a store; passing one on is the new error |
| — | new: moving a field out of a value with a `Drop` instance |
| — | new: calling `Drop` directly |

About 50 tests check the first four today. Each becomes a program that
runs, checked by its output and by valgrind (below), except those whose
point was the error, which are rewritten to show the drop.

## Testing

The compiled programs aren't built with ASan, so a test that a drop
frees runs the program under `valgrind --leak-check=full
--error-exitcode=1`, a lit feature like `cuda` (`REQUIRES: valgrind`).
Each step adds such tests next to the MIR tests of the pass
(`e2e-tests/mir/dialect/drops_*.mlir`, through `sammine-opt
-sammine-elaborate-drops`). Drop order is tested with `Drop` instances that
print.

## Steps

Each a commit, the suite passing after each.

1. `free` becomes `drop`, for owned pointers only still. A mechanical
   rename of the tests and the stdlib. (Done.)
2. `'buf<T>`: the type (lexer, types, MIR's `buffer` kind), `alloc_buf`,
   unsafe indexing, `drop(b)` frees. `Vec`, `String`, `io` and `str` move
   their buffers to it; `grow_buf` drops `old_d`. (Done.)
3. The box rule: a `'ptr` passed, returned or stored holds a value;
   parameters' and calls' pointees are live. The new error, its tests, and
   fixes to test code that breaks the rule. (Done; none broke. It also
   fixed a value stored in some branches only being taken for empty:
   dropped, overwritten or returned so, it leaked unreported.)
4. `sammine.drop`, its verifier and its lowering for `'buf<T>` and for
   `'ptr<T>` where `T` owns nothing. MIR tests. (Done: `sammine.free`
   renamed, as `drop(x)` lowers to it.)
5. `io::File`: the file handle in a struct, so no pointer drop frees it.
   (Done.)
6. `sammine-elaborate-drops`, at returns and on edges, for those
   pointers, before the check, which checks what it leaves. The leak tests
   for them become programs that run. (Done. 32 tests became `drop_*`
   programs run under valgrind; `generic_instance_per_linearity` checks a
   use after a move instead of a leak. A box what it points at was moved
   out of drops too: its drop is a free.)
7. (Folded into 6: joins and loop turns are edges.)
8. `Drop<T>`: the typeclass, the generated function calling it before the
   fields, its two new errors; a value with an instance drops; `File`'s
   closes, `Vec<T>`'s and `String`'s replace their `delete`; the
   Monomorphizer copies a generic instance for each type instantiated.
   Before struct drops, so no `File` or `Vec` drops field by field.
   (Done, with 9, differently: `Drop(x: T)` takes the value, not a view,
   and must use up what it owns; nothing is dropped after it. A view
   can't move `File`'s handle out to `close`, and the field drop after
   would free the `FILE *`: Rust's file holds a raw descriptor, with no
   drop, which sammine has no field type for. Inside `T`'s own `Drop`, a
   `T` drops field by field, or it would call itself. Calling `Drop` is
   no error: it uses the value up, as `drop(x)` does. `instantiate_drops`
   copies a generic instance for each instantiated type, after checking.
   `delete` stays, as `drop`.)
9. Structs, tuples, arrays: generated drop functions; partial moves;
   `drop(x)` of any type, a no-op for one that owns nothing. (Done for
   structs and tuples: no generated functions, `expandDrops` turns a
   struct's drop into its instance's call or its fields' drops before
   LLVM. Elaboration drops what of a partly moved one is left. A value
   with an instance moved in part isn't dropped: the check reports what
   is left. Arrays come with enums.)
10. Enums: by tag, or by the variant a case arm knows. (Done: whole, by
    tag, in `expandDrops`, with the layout moved onto MIR types; a case
    arm's bindings drop as any. Still to come: a wildcard arm's unnamed
    payloads, and arrays, whose elements the check follows as one.)
11. `'ptr<T>` whose `T` owns memory: drop `*p`, then free. (Done for the
    drops elaboration inserts. `drop(p)` the program writes still wants
    `*p` gone first.)
12. Stores, shadowing, temporaries, and a wildcard arm's unnamed payloads.
13. The leak diagnostics and their tests go; indexing a `'ptr` goes; "linear"
    becomes "owning" in the diagnostics that stay; docs.

## Decided

- `'buf<T>` and `alloc_buf<T>(n)`, as spelled above.
- `alloc<T>(1)` keeps its count for now; a `'ptr` it makes is one value
  whatever the count says, and dropping the count is left for later.
- Generic code needs nothing of its own: MIR lowers only monomorphized
  copies (`ModuleGen.cpp`, generic functions and instances have none), so
  the drop pass sees concrete types. What it needs is the copy of a generic
  `Drop` instance for each type it drops, `Drop<Vec<'ptr<i32>>>`, which no
  source calls: the Monomorphizer instantiates a type's generic `Drop`
  instance along with the type (step 8).
