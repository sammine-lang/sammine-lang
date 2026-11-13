// RUN: %sammine_opt %s -split-input-file -sammine-check-views -verify-diagnostics

// Writing through a view, at any part of what it points at, is reported
// where the view is gone through.
func.func @write_through_view(%p: !sammine.ptr<!sammine.struct<"P", (v: i32)>, view>, %q: !sammine.ptr<i32, view>) attributes {sammine.mir} {
  %c = arith.constant 2 : i32
  // expected-error @+1 {{Cannot write through non-owning pointer — only an owned pointer can be written}}
  %place = sammine.deref %p : <!sammine.struct<"P", (v: i32)>, view>
  %field = sammine.field %place[0] : <!sammine.struct<"P", (v: i32)>> -> <i32>
  sammine.store %c, %field : <i32>
  // expected-error @+1 {{Cannot write through non-owning pointer — only an owned pointer can be written}}
  %at = sammine.deref %q : <i32, view>
  %i = arith.constant 1 : i32
  %elem = sammine.ptr_elem %at[%i] : <i32>, i32
  sammine.store %c, %elem : <i32>
  return
}

// -----

// Through a view held in a slot, the slot names it.
func.func @write_through_held_view(%q: !sammine.ptr<i32, view>) attributes {sammine.mir} {
  %slot = sammine.local "q" : <!sammine.ptr<i32, view>>
  sammine.store %q, %slot : <!sammine.ptr<i32, view>>
  // expected-error @+1 {{Cannot write through non-owning pointer 'q' — only an owned pointer can be written}}
  %at = sammine.through %slot : <!sammine.ptr<i32, view>> -> <i32>
  %c = arith.constant 2 : i32
  sammine.store %c, %at : <i32>
  return
}

// -----

// An owner writes what it points at; a view of a slot reads it.
func.func @fine(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  %at = sammine.deref %p : <i32, owned>
  %c = arith.constant 2 : i32
  sammine.store %c, %at : <i32>
  %x = sammine.local "x" : <i32>
  sammine.store %c, %x : <i32>
  %view = sammine.addr %x : <i32>
  sammine.drop %p : !sammine.ptr<i32, owned>
  return
}

// -----

// & of what an owned pointer points at could outlive its drop.
func.func @addr_of_owned_memory(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  // expected-error @+1 {{Cannot take the address of memory an owned pointer holds: it could dangle once that is dropped}}
  %at = sammine.deref %p : <i32, owned>
  %view = sammine.addr %at : <i32>
  sammine.drop %p : !sammine.ptr<i32, owned>
  return
}

// -----

// & of an owned pointer would alias its one owner.
func.func @addr_of_owner(%n: i32) attributes {sammine.mir} {
  %slot = sammine.local "a" : <!sammine.ptr<i32, owned>>
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  sammine.store %0, %slot : <!sammine.ptr<i32, owned>>
  // expected-error @+1 {{Cannot take the address of an owned pointer: it would alias its one owner}}
  %view = sammine.addr %slot : <!sammine.ptr<i32, owned>>
  return
}

// -----

// A closure binds no value that owns memory: it could be called twice.
func.func private @take(%p: !sammine.ptr<i32, owned>, %x: i32) -> i32
func.func @binds_owner(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  // expected-error @+1 {{Cannot bind 'p' that owns memory into a partial application of 'take': the closure can be called more than once}}
  %f = sammine.closure @take(%p) : (!sammine.ptr<i32, owned>) -> !sammine.fn<(i32) -> i32>
  return
}

// -----

// A closure that bound a view is followed into what is built of it and
// across blocks, to the return.
func.func private @peek(%p: !sammine.ptr<i32, view>, %x: i32) -> i32
func.func @escapes(%c: i1, %v: !sammine.ptr<i32, view>) -> !sammine.tuple<!sammine.fn<(i32) -> i32>> attributes {sammine.mir} {
  %f = sammine.closure @peek(%v) : (!sammine.ptr<i32, view>) -> !sammine.fn<(i32) -> i32>
  cf.br ^bb1(%f : !sammine.fn<(i32) -> i32>)
^bb1(%g: !sammine.fn<(i32) -> i32>):
  %t = sammine.aggregate(%g) : (!sammine.fn<(i32) -> i32>) -> !sammine.tuple<!sammine.fn<(i32) -> i32>>
  // expected-error @+1 {{Cannot return a partial application of 'peek' that captured a view (ptr<T>) (may dangle after return)}}
  return %t : !sammine.tuple<!sammine.fn<(i32) -> i32>>
}

// -----

// Called where it was made, it leaves nothing behind.
func.func private @peek(%p: !sammine.ptr<i32, view>, %x: i32) -> i32
func.func @stays(%v: !sammine.ptr<i32, view>, %x: i32) -> i32 attributes {sammine.mir} {
  %f = sammine.closure @peek(%v) : (!sammine.ptr<i32, view>) -> !sammine.fn<(i32) -> i32>
  %r = sammine.call_indirect %f(%x) : <(i32) -> i32>, (i32) -> i32
  return %r : i32
}
