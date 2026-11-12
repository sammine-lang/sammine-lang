// RUN: %sammine_opt %s -split-input-file -verify-diagnostics

func.func @not_aggregate(%p: !sammine.place<i32>) {
  // expected-error @+1 {{'sammine.field' op names a field of a struct or tuple, not 'i32'}}
  %f = sammine.field %p[0] : !sammine.place<i32> -> !sammine.place<i32>
  return
}

// -----

func.func @wrong_field(%p: !sammine.place<!sammine.tuple<i32, i64>>) {
  // expected-error @+1 {{'sammine.field' op gives a place of 'i32' for field 1, which is 'i64'}}
  %f = sammine.field %p[1] : !sammine.place<!sammine.tuple<i32, i64>> -> !sammine.place<i32>
  return
}

// -----

func.func @not_array(%p: !sammine.place<i32>, %i: i32) {
  // expected-error @+1 {{'sammine.elem' op names an element of an array, not 'i32'}}
  %e = sammine.elem %p[%i] : !sammine.place<i32>, i32 -> !sammine.place<i32>
  return
}
