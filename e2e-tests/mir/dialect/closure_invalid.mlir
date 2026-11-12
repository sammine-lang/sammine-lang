// RUN: %sammine_opt %s -split-input-file -verify-diagnostics

func.func private @add(%a: i32, %b: i32) -> i32 attributes {sammine.mir} {
  return %a : i32
}

func.func @too_many(%x: i32) {
  // expected-error @+1 {{'sammine.closure' op binds 2 of 'add', whose 2 arguments don't fit '(i32) -> i32'}}
  %f = sammine.closure @add(%x, %x) : (i32, i32) -> !sammine.fn<(i32) -> i32>
  return
}

// -----

func.func @wrong_args(%f: !sammine.fn<(i32) -> i32>, %x: i64) {
  // expected-error @+1 {{'sammine.call_indirect' op passes and takes back '(i64) -> i32', but the function value is '(i32) -> i32'}}
  %r = sammine.call_indirect %f(%x) : !sammine.fn<(i32) -> i32>, (i64) -> i32
  return
}
