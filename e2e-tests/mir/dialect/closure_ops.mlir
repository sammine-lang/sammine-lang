// RUN: %sammine_opt %s | %sammine_opt | %check %s
// Function values, binding some arguments or none, and calls through them
// print as they parse.

func.func private @add(%a: i32, %b: i32) -> i32 attributes {sammine.mir} {
  %s = arith.addi %a, %b : i32
  return %s : i32
}

// CHECK-LABEL: func.func @f(
func.func @f(%x: i32) -> i32 {
  // CHECK: sammine.closure @add() : () -> !sammine.fn<(i32, i32) -> i32>
  %all = sammine.closure @add() : () -> !sammine.fn<(i32, i32) -> i32>
  // CHECK: sammine.closure @add(%arg0) : (i32) -> !sammine.fn<(i32) -> i32>
  %one = sammine.closure @add(%x) : (i32) -> !sammine.fn<(i32) -> i32>
  // CHECK: sammine.call_indirect %1(%arg0) : <(i32) -> i32>, (i32) -> i32
  %r = sammine.call_indirect %one(%x) : !sammine.fn<(i32) -> i32>, (i32) -> i32
  return %r : i32
}
