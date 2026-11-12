// RUN: %sammine_opt %s -split-input-file -verify-diagnostics

func.func @no_callee() {
  // expected-error @+1 {{'sammine.call' op calls 'missing', which isn't a function}}
  sammine.call @missing() : () -> ()
  return
}

// -----

func.func private @two(%a: i32, %b: i32) -> i32

func.func @wrong_count(%a: i32) -> i32 {
  // expected-error @+1 {{'sammine.call' op passes 1 and takes back 1, but 'two' takes 2 and gives back 1}}
  %r = sammine.call @two(%a) : (i32) -> i32
  return %r : i32
}

// -----

func.func private @owned(%p: !sammine.ptr<i32, owned>) attributes {sammine.mir} {
  return
}

// A callee through MIR takes exactly MIR's types: a view isn't an owner.
func.func @wrong_type(%p: !sammine.ptr<i32, view>) {
  // expected-error @+1 {{'sammine.call' op types don't match those of 'owned': '(!sammine.ptr<i32, owned>) -> ()'}}
  sammine.call @owned(%p) : (!sammine.ptr<i32, view>) -> ()
  return
}

// -----

llvm.func @printf(!llvm.ptr, ...) -> i32

// A variadic C function takes at least the arguments it declares.
func.func @too_few() -> i32 {
  // expected-error @+1 {{'sammine.call' op passes 0 and takes back 1, but 'printf' takes at least 1 and gives back 1}}
  %n = sammine.call @printf() : () -> i32
  return %n : i32
}
