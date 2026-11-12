// RUN: %sammine_opt %s -split-input-file -verify-diagnostics

func.func @wrong_field(%a: i64) {
  // expected-error @+1 {{'sammine.aggregate' op takes the types of the fields of '!sammine.struct<"P", (i32)>'}}
  %s = sammine.aggregate(%a) : (i64) -> !sammine.struct<"P", (i32)>
  return
}

// -----

func.func @not_aggregate(%a: i32) {
  // expected-error @+1 {{'sammine.aggregate' op builds a struct or tuple, not 'i32'}}
  %s = sammine.aggregate(%a) : (i32) -> i32
  return
}

// -----

func.func @past_end(%t: !sammine.tuple<i32, i32>) {
  // expected-error @+1 {{'sammine.extract' op reads field 2 of '!sammine.tuple<i32, i32>', which has 2}}
  %v = sammine.extract %t[2] : !sammine.tuple<i32, i32> -> i32
  return
}

// -----

func.func @wrong_result(%t: !sammine.tuple<i32, i64>) {
  // expected-error @+1 {{'sammine.extract' op gives field 1 as 'i32', but it is 'i64'}}
  %v = sammine.extract %t[1] : !sammine.tuple<i32, i64> -> i32
  return
}
