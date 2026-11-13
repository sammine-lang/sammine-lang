// RUN: %sammine_opt %s -split-input-file -verify-diagnostics

func.func @copy_owned(%pp: !sammine.ptr<!sammine.ptr<i32, owned>, view>) {
  %place = sammine.deref %pp : !sammine.ptr<!sammine.ptr<i32, owned>, view>
  // expected-error @+1 {{'sammine.copy' op can't copy a value that owns memory; use sammine.move}}
  %q = sammine.copy %place : !sammine.place<!sammine.ptr<i32, owned>>
  return
}

// -----

func.func @move_plain(%p: !sammine.ptr<i32, view>) {
  %place = sammine.deref %p : !sammine.ptr<i32, view>
  // expected-error @+1 {{'sammine.move' op moves only a value that owns memory; use sammine.copy}}
  %v = sammine.move %place : !sammine.place<i32>
  return
}

// -----

func.func @drop_view(%p: !sammine.ptr<i32, view>) {
  // expected-error @+1 {{owned pointer}}
  sammine.drop %p : !sammine.ptr<i32, view>
  return
}

// -----

// A lent place is only viewed: the result can't own it.
func.func @lend_owned(%p: !sammine.ptr<i32, owned>) {
  %place = sammine.deref %p : !sammine.ptr<i32, owned>
  // expected-error @+1 {{'sammine.lend' op failed to verify that the result is a view of what the place holds}}
  %v = "sammine.lend"(%place) : (!sammine.place<i32>) -> !sammine.ptr<i32, owned>
  return
}

// -----

// A view lasts as long as the call it is passed to: it can't outlive it.
func.func @lend_returned(%p: !sammine.ptr<i32, owned>) -> !sammine.ptr<i32, view> {
  %place = sammine.deref %p : !sammine.ptr<i32, owned>
  // expected-error @+1 {{'sammine.lend' op is a view lent to a call or a closure, so only sammine.call or sammine.closure can use it, not func.return}}
  %v = sammine.lend %place : !sammine.place<i32>
  return %v : !sammine.ptr<i32, view>
}
