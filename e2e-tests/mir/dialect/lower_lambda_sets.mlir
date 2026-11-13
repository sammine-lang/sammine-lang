// RUN: %sammine_opt %s -sammine-lower-lambda-sets | %check %s
// A function value whose lambda set is known becomes what its closure
// binds: a tuple for one closure, an enum of them for several. A call
// through it is a direct call of its closure's function, given what the
// closure binds then the arguments; with several, a switch on which.

func.func private @add(%a: i32, %b: i32) -> i32 attributes {sammine.mir} {
  %s = arith.addi %a, %b : i32
  return %s : i32
}
func.func private @neg(%b: i32) -> i32 attributes {sammine.mir} {
  %z = arith.constant 0 : i32
  %s = arith.subi %z, %b : i32
  return %s : i32
}

// CHECK-LABEL: func.func private @one(%arg0: i32) -> i32
// CHECK: %[[F:.*]] = sammine.aggregate(%arg0) {sammine.captures} : (i32) -> !sammine.tuple<i32>
// CHECK: %[[N:.*]] = sammine.extract %[[F]][0]
// CHECK: sammine.call @add(%[[N]], %arg0)
func.func private @one(%x: i32) -> i32 attributes {sammine.mir} {
  %f = sammine.closure @add(%x) : (i32) -> !sammine.fn<(i32) -> i32, 1>
  %r = sammine.call_indirect %f(%x) : !sammine.fn<(i32) -> i32, 1>, (i32) -> i32
  return %r : i32
}

// CHECK-LABEL: func.func private @two(%arg0: i1, %arg1: i32) -> i32
// CHECK: sammine.variant 1(%arg1) at [0] {sammine.captures} : (i32) -> !sammine.enum<"lambdas{{[0-9]+}}", 4, (closure0: !sammine.tuple<>, closure1: !sammine.tuple<i32>)>
// CHECK: sammine.variant 0() at [] {sammine.captures}
// CHECK: sammine.tag
// CHECK: cf.switch
// CHECK: sammine.call @neg(%arg1)
// CHECK: sammine.payload
// CHECK: sammine.call @add(
// CHECK: sammine.unreachable
func.func private @two(%b: i1, %x: i32) -> i32 attributes {sammine.mir} {
  cf.cond_br %b, ^plus, ^minus
^plus:
  %p = sammine.closure @add(%x) : (i32) -> !sammine.fn<(i32) -> i32, 1>
  cf.br ^call(%p : !sammine.fn<(i32) -> i32, 1>)
^minus:
  %m = sammine.closure @neg() : () -> !sammine.fn<(i32) -> i32, 1>
  cf.br ^call(%m : !sammine.fn<(i32) -> i32, 1>)
^call(%f: !sammine.fn<(i32) -> i32, 1>):
  %r = sammine.call_indirect %f(%x) : !sammine.fn<(i32) -> i32, 1>, (i32) -> i32
  return %r : i32
}
