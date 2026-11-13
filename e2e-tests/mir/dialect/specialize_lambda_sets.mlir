// RUN: %sammine_opt %s -sammine-specialize-lambda-sets -sammine-lower-lambda-sets | %check %s
// A higher-order function is copied per lambda set it is used with: each
// copy calls its one closure's function directly, with no switch. Places
// using it with alike sets share a copy, and one calling itself does so in
// its own copy.

func.func private @add(%a: i32, %b: i32) -> i32 attributes {sammine.mir} {
  %s = arith.addi %a, %b : i32
  return %s : i32
}
func.func private @mul(%a: i32, %b: i32) -> i32 attributes {sammine.mir} {
  %s = arith.muli %a, %b : i32
  return %s : i32
}

// CHECK-LABEL: func.func private @apply(%arg0: !sammine.tuple<i32>, %arg1: i32) -> i32
// CHECK-NOT: cf.switch
// CHECK: sammine.call @add(
// CHECK-LABEL: func.func private @apply.spec{{[0-9]+}}(%arg0: !sammine.tuple<i32>, %arg1: i32) -> i32
// CHECK-NOT: cf.switch
// CHECK: sammine.call @mul(
// CHECK-NOT: func.func private @apply.spec
func.func private @apply(%f: !sammine.fn<(i32) -> i32, 1>, %x: i32) -> i32 attributes {sammine.mir} {
  %r = sammine.call_indirect %f(%x) : !sammine.fn<(i32) -> i32, 1>, (i32) -> i32
  return %r : i32
}

// Calls itself n times: the recursive call stays in its copy.
// CHECK-LABEL: func.func private @times(%arg0: !sammine.tuple<i32>, %arg1: i32, %arg2: i32) -> i32
// CHECK: sammine.call @times(
// CHECK: sammine.call @add(
// CHECK-LABEL: func.func private @times.spec{{[0-9]+}}(%arg0: !sammine.tuple<i32>, %arg1: i32, %arg2: i32) -> i32
// CHECK: sammine.call @times.spec{{[0-9]+}}(
// CHECK: sammine.call @mul(
func.func private @times(%f: !sammine.fn<(i32) -> i32, 1>, %n: i32, %x: i32) -> i32 attributes {sammine.mir} {
  %zero = arith.constant 0 : i32
  %done = arith.cmpi eq, %n, %zero : i32
  cf.cond_br %done, ^end, ^step
^end:
  return %x : i32
^step:
  %one = arith.constant 1 : i32
  %m = arith.subi %n, %one : i32
  %y = sammine.call @times(%f, %m, %x) : (!sammine.fn<(i32) -> i32, 1>, i32, i32) -> i32
  %r = sammine.call_indirect %f(%y) : !sammine.fn<(i32) -> i32, 1>, (i32) -> i32
  return %r : i32
}

// CHECK-LABEL: func.func @main
// CHECK: sammine.call @apply(
// CHECK: sammine.call @apply.spec{{[0-9]+}}(
// CHECK: sammine.call @apply(
// CHECK: sammine.call @times(
// CHECK: sammine.call @times.spec{{[0-9]+}}(
func.func @main(%x: i32) -> i32 attributes {sammine.mir} {
  %one = arith.constant 1 : i32
  %two = arith.constant 2 : i32
  %f = sammine.closure @add(%one) : (i32) -> !sammine.fn<(i32) -> i32, 1>
  %g = sammine.closure @mul(%two) : (i32) -> !sammine.fn<(i32) -> i32, 1>
  %h = sammine.closure @add(%two) : (i32) -> !sammine.fn<(i32) -> i32, 1>
  %a = sammine.call @apply(%f, %x) : (!sammine.fn<(i32) -> i32, 1>, i32) -> i32
  %b = sammine.call @apply(%g, %a) : (!sammine.fn<(i32) -> i32, 1>, i32) -> i32
  // Alike to the first: it shares that copy.
  %c = sammine.call @apply(%h, %b) : (!sammine.fn<(i32) -> i32, 1>, i32) -> i32
  %d = sammine.call @times(%f, %two, %c) : (!sammine.fn<(i32) -> i32, 1>, i32, i32) -> i32
  %e = sammine.call @times(%g, %two, %d) : (!sammine.fn<(i32) -> i32, 1>, i32, i32) -> i32
  return %e : i32
}
