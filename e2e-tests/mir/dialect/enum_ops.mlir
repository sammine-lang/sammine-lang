// RUN: %sammine_opt %s | %sammine_opt | %check %s
// An enum type, building a variant, reading its tag and a payload field print
// as they parse.

// CHECK-LABEL: func.func @f(
func.func @f(%x: i64) -> i64 {
  // CHECK: sammine.variant 0(%arg0) at [0] : (i64) -> !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>
  %some = sammine.variant 0(%x) at [0] : (i64) -> !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>
  // CHECK: sammine.variant 1() at [] : () -> !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>
  %none = sammine.variant 1() at [] : () -> !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>
  // CHECK: sammine.tag %{{.*}} : <"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>
  %tag = sammine.tag %some : !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>
  // CHECK: sammine.payload %{{.*}}[0, 0] at 0 : <"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)> -> i64
  %v = sammine.payload %some[0, 0] at 0 : !sammine.enum<"Opt", 8, (!sammine.tuple<i64>, !sammine.tuple<>)> -> i64
  return %v : i64
}
