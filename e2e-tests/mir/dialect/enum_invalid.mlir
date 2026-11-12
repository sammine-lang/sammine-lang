// RUN: %sammine_opt %s -split-input-file -verify-diagnostics

func.func @no_such_variant() {
  // expected-error @+1 {{'sammine.variant' op builds variant 2 of '!sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>', which has 2}}
  %e = sammine.variant 2() at [] : () -> !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>
  return
}

// -----

func.func @wrong_payload(%x: i32) {
  // expected-error @+1 {{'sammine.variant' op takes the payload of variant 0}}
  %e = sammine.variant 0(%x) at [0] : (i32) -> !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>
  return
}

// -----

func.func @missing_offset(%x: i64) {
  // expected-error @+1 {{'sammine.variant' op needs an offset for each payload field}}
  %e = sammine.variant 0(%x) at [] : (i64) -> !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>
  return
}

// -----

func.func @no_such_field(%e: !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>) {
  // expected-error @+1 {{'sammine.payload' op reads field 0 of variant 1's payload, which has 0}}
  %v = sammine.payload %e[1, 0] at 0 : !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)> -> i64
  return
}
