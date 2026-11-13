// RUN: %sammine_opt %s -sammine-size-closures | %check %s
// A function type carries the words the largest set of values any closure
// of it binds takes, read off the module's closures: MIR is made with none.
// Every type holding one follows, and an enum's size and the offsets into
// its payloads with it. A closure binding a value of its own type can't
// carry it: it counts for one word, a pointer to what it binds.

func.func private @add(%a: i32, %b: i32) -> i32 attributes {sammine.mir} {
  %s = arith.addi %a, %b : i32
  return %s : i32
}
func.func private @add3(%a: i64, %b: i64, %c: i32) -> i32 attributes {sammine.mir} {
  return %c : i32
}

// Both closures are of (i32) -> i32: it carries the larger, two i64.
// CHECK-LABEL: func.func private @sized(%arg0: i32, %arg1: i64) -> !sammine.fn<(i32) -> i32, 2>
// CHECK: sammine.closure @add(%arg0) : (i32) -> !sammine.fn<(i32) -> i32, 2>
// CHECK: sammine.closure @add3(%arg1, %arg1) : (i64, i64) -> !sammine.fn<(i32) -> i32, 2>
func.func private @sized(%x: i32, %y: i64) -> !sammine.fn<(i32) -> i32> attributes {sammine.mir} {
  %one = sammine.closure @add(%x) : (i32) -> !sammine.fn<(i32) -> i32>
  %two = sammine.closure @add3(%y, %y) : (i64, i64) -> !sammine.fn<(i32) -> i32>
  return %two : !sammine.fn<(i32) -> i32>
}

// No closure of (i32, i32) -> i32 binds anything: it stays its code.
// CHECK-LABEL: func.func private @unbound() -> !sammine.fn<(i32, i32) -> i32>
func.func private @unbound() -> !sammine.fn<(i32, i32) -> i32> attributes {sammine.mir} {
  %all = sammine.closure @add() : () -> !sammine.fn<(i32, i32) -> i32>
  return %all : !sammine.fn<(i32, i32) -> i32>
}

// An enum holding one grows with it, and its fields move.
// CHECK-LABEL: func.func private @held(
// CHECK: sammine.variant 0(%arg1, %arg0) at [0, 24] : (i32, !sammine.fn<(i32) -> i32, 2>) -> !sammine.enum<"Job", 48, (Run: !sammine.tuple<i32, !sammine.fn<(i32) -> i32, 2>>, Idle: !sammine.tuple<>)>
// CHECK: sammine.payload %{{.*}}[0, 1] at 24
func.func private @held(%f: !sammine.fn<(i32) -> i32>, %n: i32) -> !sammine.fn<(i32) -> i32> attributes {sammine.mir} {
  %job = sammine.variant 0(%n, %f) at [0, 8] : (i32, !sammine.fn<(i32) -> i32>) -> !sammine.enum<"Job", 16, (Run: !sammine.tuple<i32, !sammine.fn<(i32) -> i32>>, Idle: !sammine.tuple<>)>
  %g = sammine.payload %job[0, 1] at 8 : <"Job", 16, (Run: !sammine.tuple<i32, !sammine.fn<(i32) -> i32>>, Idle: !sammine.tuple<>)> -> !sammine.fn<(i32) -> i32>
  return %g : !sammine.fn<(i32) -> i32>
}

func.func private @nest(%f: !sammine.fn<(i64) -> i64>, %x: i64) -> i64 attributes {sammine.mir} {
  return %x : i64
}

// nest(f) binds a value of the type it is: one word, for a pointer.
// CHECK-LABEL: func.func private @self(%arg0: !sammine.fn<(i64) -> i64, 1>) -> !sammine.fn<(i64) -> i64, 1>
// CHECK: sammine.closure @nest(%arg0) : (!sammine.fn<(i64) -> i64, 1>) -> !sammine.fn<(i64) -> i64, 1>
func.func private @self(%f: !sammine.fn<(i64) -> i64>) -> !sammine.fn<(i64) -> i64> attributes {sammine.mir} {
  %g = sammine.closure @nest(%f) : (!sammine.fn<(i64) -> i64>) -> !sammine.fn<(i64) -> i64>
  return %g : !sammine.fn<(i64) -> i64>
}
