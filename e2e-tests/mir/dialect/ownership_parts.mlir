// RUN: %sammine_opt %s -split-input-file -sammine-check-ownership -verify-diagnostics
// The ownership check follows parts: a struct's fields, an enum's payloads,
// what an owned pointer points at, and places.

func.func private @take(!sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>)

func.func @part_then_whole(%b0: !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>) attributes {sammine.mir} {
  %b = sammine.let "b" %b0 : !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>
  %p = sammine.extract %b[0] : !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)> -> !sammine.ptr<i32, owned>
  // expected-note @+1 {{'b.p' was dropped here}}
  sammine.drop %p : !sammine.ptr<i32, owned>
  // expected-error @+1 {{Cannot move 'b' — its part 'b.p' was already dropped}}
  sammine.call @take(%b) : (!sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>) -> ()
  return
}

// -----

func.func @part_left(%b0: !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>) attributes {sammine.mir} {
  // expected-error @+1 {{'b.q' is still owned where it goes out of use}}
  %b = sammine.let "b" %b0 : !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>
  %p = sammine.extract %b[0] : !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)> -> !sammine.ptr<i32, owned>
  sammine.drop %p : !sammine.ptr<i32, owned>
  return
}

// -----

// What alloc gives points at nothing until something is stored there.
func.func @nothing_stored(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  // expected-error @+1 {{Cannot use '*pp' — no owned value is known to be stored there}}
  %inner = sammine.move %place : <!sammine.ptr<i32, owned>>
  sammine.drop %inner : !sammine.ptr<i32, owned>
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}

// -----

// Stored, taken back out, and both freed.
func.func @stored_and_taken(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %1 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  sammine.store %1, %place : <!sammine.ptr<i32, owned>>
  %again = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  %inner = sammine.move %again : <!sammine.ptr<i32, owned>>
  sammine.drop %inner : !sammine.ptr<i32, owned>
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}

// -----

func.func @store_over_live(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %1 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %2 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  sammine.store %1, %place : <!sammine.ptr<i32, owned>>
  // expected-error @+1 {{Storing into '*pp' while it still owns a value}}
  sammine.store %2, %place : <!sammine.ptr<i32, owned>>
  %inner = sammine.move %place : <!sammine.ptr<i32, owned>>
  sammine.drop %inner : !sammine.ptr<i32, owned>
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}

// -----

// Freeing an owner frees only its memory: what it points at must be gone.
func.func @free_with_live_pointee(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %1 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  // expected-error @+1 {{'*pp' is still owned where it goes out of use}}
  sammine.store %1, %place : <!sammine.ptr<i32, owned>>
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}

// -----

// A dead pointer is reported at the pointer, not again at what it pointed at.
func.func @through_freed_pointer(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // expected-note @+1 {{'pp' was dropped here}}
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // expected-error @+1 {{Cannot dereference 'pp' — it was already dropped previously}}
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  %inner = sammine.move %place : <!sammine.ptr<i32, owned>>
  sammine.drop %inner : !sammine.ptr<i32, owned>
  return
}

// -----

// Each arm has only its variant's payload, which its binding takes.
func.func @case_arms(%e0: !sammine.enum<"Opt", 8, (Some: !sammine.tuple<!sammine.ptr<i32, owned>>, None: !sammine.tuple<>)>) attributes {sammine.mir} {
  %e = sammine.let "e" %e0 : !sammine.enum<"Opt", 8, (Some: !sammine.tuple<!sammine.ptr<i32, owned>>, None: !sammine.tuple<>)>
  %t = sammine.tag %e : <"Opt", 8, (Some: !sammine.tuple<!sammine.ptr<i32, owned>>, None: !sammine.tuple<>)>
  cf.switch %t : i32, [
    default: ^none,
    0: ^some
  ]
^some:
  %0 = sammine.payload %e[0, 0] at 0 : <"Opt", 8, (Some: !sammine.tuple<!sammine.ptr<i32, owned>>, None: !sammine.tuple<>)> -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  sammine.drop %p : !sammine.ptr<i32, owned>
  cf.br ^done
^none:
  cf.br ^done
^done:
  return
}

// -----

func.func @case_twice(%e0: !sammine.enum<"Opt", 8, (Some: !sammine.tuple<!sammine.ptr<i32, owned>>, None: !sammine.tuple<>)>) attributes {sammine.mir} {
  %e = sammine.let "e" %e0 : !sammine.enum<"Opt", 8, (Some: !sammine.tuple<!sammine.ptr<i32, owned>>, None: !sammine.tuple<>)>
  // expected-note @+1 {{'e' was matched in case here}}
  %t = sammine.tag %e : <"Opt", 8, (Some: !sammine.tuple<!sammine.ptr<i32, owned>>, None: !sammine.tuple<>)>
  cf.switch %t : i32, [
    default: ^none,
    0: ^some
  ]
^some:
  %0 = sammine.payload %e[0, 0] at 0 : <"Opt", 8, (Some: !sammine.tuple<!sammine.ptr<i32, owned>>, None: !sammine.tuple<>)> -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  sammine.drop %p : !sammine.ptr<i32, owned>
  cf.br ^done
^none:
  cf.br ^done
^done:
  // expected-error @+1 {{Cannot use 'e' — it was already matched in case previously}}
  %t2 = sammine.tag %e : <"Opt", 8, (Some: !sammine.tuple<!sammine.ptr<i32, owned>>, None: !sammine.tuple<>)>
  return
}

// -----

func.func private @both(!sammine.ptr<i32, view>, !sammine.ptr<i32, owned>)

// The callee could free what it was given while still reading the view.
func.func @lend_and_move(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  %place = sammine.deref %p : <i32, owned>
  %view = sammine.lend %place : <i32>
  // expected-error @+1 {{Cannot lend 'p' and move it in the same call}}
  sammine.call @both(%view, %p) : (!sammine.ptr<i32, view>, !sammine.ptr<i32, owned>) -> ()
  return
}

// -----

// `**pp = v`: through the pointer *pp holds, which stays there.
func.func @through_held_pointer(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<!sammine.ptr<i32, owned>, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<!sammine.ptr<i32, owned>, owned>, owned>
  %1 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %held = sammine.deref %pp : <!sammine.ptr<!sammine.ptr<i32, owned>, owned>, owned>
  sammine.store %1, %held : <!sammine.ptr<!sammine.ptr<i32, owned>, owned>>
  %2 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %again = sammine.deref %pp : <!sammine.ptr<!sammine.ptr<i32, owned>, owned>, owned>
  %inner = sammine.through %again : <!sammine.ptr<!sammine.ptr<i32, owned>, owned>> -> <!sammine.ptr<i32, owned>>
  // expected-error @+1 {{'**pp' is still owned where it goes out of use}}
  sammine.store %2, %inner : <!sammine.ptr<i32, owned>>
  %last = sammine.deref %pp : <!sammine.ptr<!sammine.ptr<i32, owned>, owned>, owned>
  %taken = sammine.move %last : <!sammine.ptr<!sammine.ptr<i32, owned>, owned>>
  sammine.drop %taken : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<!sammine.ptr<i32, owned>, owned>, owned>
  return
}

// -----

// A buffer's values are its owner's to count, so the check doesn't follow
// them: freeing a buffer of owned pointers reports nothing, unlike freeing a
// box whose pointer is still there (@free_with_live_pointee).
func.func @free_buffer_of_owners(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, buffer>
  %b = sammine.let "b" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, buffer>
  %1 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %place = sammine.deref %b : <!sammine.ptr<i32, owned>, buffer>
  sammine.store %1, %place : <!sammine.ptr<i32, owned>>
  sammine.drop %b : !sammine.ptr<!sammine.ptr<i32, owned>, buffer>
  return
}

// -----

// A parameter's pointee holds a value: its caller put one there. Freeing
// only the outer pointer leaks it.
func.func @param_pointee_live(%pp: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) attributes {sammine.mir} {
  // expected-error @-1 {{Temporary owning memory must not be discarded (bind it with 'let' first)}}
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}

// -----

func.func private @boxed() -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>

// So does a call's result's: what it points at can be moved out.
func.func @call_result_pointee_live() attributes {sammine.mir} {
  %0 = sammine.call @boxed() : () -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  %p = sammine.move %place : <!sammine.ptr<i32, owned>>
  sammine.drop %p : !sammine.ptr<i32, owned>
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}

// -----

// What alloc gives points at nothing, so it can't leave the function so.
func.func @return_empty(%n: i32) -> !sammine.ptr<!sammine.ptr<i32, owned>, owned> attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // expected-error @+1 {{'pp' must point at a value when it is returned, but '*pp' holds nothing yet}}
  return %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
}

// -----

func.func private @take(!sammine.ptr<!sammine.ptr<i32, owned>, owned>)

// Nor be passed on once what it pointed at was moved out.
func.func @pass_after_move(%pp: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) attributes {sammine.mir} {
  %pp1 = sammine.let "pp" %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %place = sammine.deref %pp1 : <!sammine.ptr<i32, owned>, owned>
  // expected-note @+1 {{'*pp' was moved here}}
  %p = sammine.move %place : <!sammine.ptr<i32, owned>>
  %q = sammine.let "q" %p : !sammine.ptr<i32, owned>
  sammine.drop %q : !sammine.ptr<i32, owned>
  // expected-error @+1 {{'pp' must point at a value when it is passed to a function, but '*pp' was moved}}
  sammine.call @take(%pp1) : (!sammine.ptr<!sammine.ptr<i32, owned>, owned>) -> ()
  return
}
