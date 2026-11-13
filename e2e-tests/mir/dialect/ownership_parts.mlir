// RUN: %sammine_opt %s -split-input-file -sammine-check-ownership -verify-diagnostics
// The ownership check follows parts: a struct's fields, an enum's payloads,
// what an owned pointer points at, and places.

func.func private @take(!sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>)

func.func @part_then_whole(%b0: !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>) attributes {sammine.mir} {
  %b = sammine.let "b" %b0 : !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>
  %p = sammine.extract %b[0] : !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)> -> !sammine.ptr<i32, owned>
  // expected-note @+1 {{'b.p' was freed here}}
  sammine.free %p : !sammine.ptr<i32, owned>
  // expected-error @+1 {{Cannot move 'b' — its part 'b.p' was already freed}}
  sammine.call @take(%b) : (!sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>) -> ()
  return
}

// -----

func.func @part_left(%b0: !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>) attributes {sammine.mir} {
  // expected-error @+1 {{Linear variable 'b.q' must be consumed before scope exit (use free() to deallocate)}}
  %b = sammine.let "b" %b0 : !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)>
  %p = sammine.extract %b[0] : !sammine.struct<"Box", (p: !sammine.ptr<i32, owned>, q: !sammine.ptr<i32, owned>)> -> !sammine.ptr<i32, owned>
  sammine.free %p : !sammine.ptr<i32, owned>
  return
}

// -----

// What alloc gives points at nothing until something is stored there.
func.func @nothing_stored(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  // expected-error @+1 {{Cannot use linear variable '*pp' — no owned value is known to be stored there}}
  %inner = sammine.move %place : <!sammine.ptr<i32, owned>>
  sammine.free %inner : !sammine.ptr<i32, owned>
  sammine.free %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
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
  sammine.free %inner : !sammine.ptr<i32, owned>
  sammine.free %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
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
  // expected-error @+1 {{Reassigning linear variable '*pp' without consuming its previous value}}
  sammine.store %2, %place : <!sammine.ptr<i32, owned>>
  %inner = sammine.move %place : <!sammine.ptr<i32, owned>>
  sammine.free %inner : !sammine.ptr<i32, owned>
  sammine.free %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}

// -----

// Freeing an owner frees only its memory: what it points at must be gone.
func.func @free_with_live_pointee(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %1 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  // expected-error @+1 {{Linear variable '*pp' must be consumed before scope exit (use free() to deallocate)}}
  sammine.store %1, %place : <!sammine.ptr<i32, owned>>
  sammine.free %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}

// -----

// A dead pointer is reported at the pointer, not again at what it pointed at.
func.func @through_freed_pointer(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %pp = sammine.let "pp" %0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // expected-note @+1 {{'pp' was freed here}}
  sammine.free %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // expected-error @+1 {{Cannot dereference 'pp' — it was already freed previously}}
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  %inner = sammine.move %place : <!sammine.ptr<i32, owned>>
  sammine.free %inner : !sammine.ptr<i32, owned>
  return
}

// -----

// Each arm has only its variant's payload, which its binding takes.
func.func @case_arms(%e0: !sammine.enum<"Opt", 8, (!sammine.tuple<!sammine.ptr<i32, owned>>, !sammine.tuple<>)>) attributes {sammine.mir} {
  %e = sammine.let "e" %e0 : !sammine.enum<"Opt", 8, (!sammine.tuple<!sammine.ptr<i32, owned>>, !sammine.tuple<>)>
  %t = sammine.tag %e : <"Opt", 8, (!sammine.tuple<!sammine.ptr<i32, owned>>, !sammine.tuple<>)>
  cf.switch %t : i32, [
    default: ^none,
    0: ^some
  ]
^some:
  %0 = sammine.payload %e[0, 0] at 0 : <"Opt", 8, (!sammine.tuple<!sammine.ptr<i32, owned>>, !sammine.tuple<>)> -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  sammine.free %p : !sammine.ptr<i32, owned>
  cf.br ^done
^none:
  cf.br ^done
^done:
  return
}

// -----

func.func @case_twice(%e0: !sammine.enum<"Opt", 8, (!sammine.tuple<!sammine.ptr<i32, owned>>, !sammine.tuple<>)>) attributes {sammine.mir} {
  %e = sammine.let "e" %e0 : !sammine.enum<"Opt", 8, (!sammine.tuple<!sammine.ptr<i32, owned>>, !sammine.tuple<>)>
  // expected-note @+1 {{'e' was matched in case here}}
  %t = sammine.tag %e : <"Opt", 8, (!sammine.tuple<!sammine.ptr<i32, owned>>, !sammine.tuple<>)>
  cf.switch %t : i32, [
    default: ^none,
    0: ^some
  ]
^some:
  %0 = sammine.payload %e[0, 0] at 0 : <"Opt", 8, (!sammine.tuple<!sammine.ptr<i32, owned>>, !sammine.tuple<>)> -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  sammine.free %p : !sammine.ptr<i32, owned>
  cf.br ^done
^none:
  cf.br ^done
^done:
  // expected-error @+1 {{Cannot use linear variable 'e' — it was already matched in case previously}}
  %t2 = sammine.tag %e : <"Opt", 8, (!sammine.tuple<!sammine.ptr<i32, owned>>, !sammine.tuple<>)>
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
