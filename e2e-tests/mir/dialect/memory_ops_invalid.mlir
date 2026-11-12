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

func.func @free_view(%p: !sammine.ptr<i32, view>) {
  // expected-error @+1 {{owned pointer}}
  sammine.free %p : !sammine.ptr<i32, view>
  return
}
