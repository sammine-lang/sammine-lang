// RUN: (! %sammine_opt %s -sammine-mir-to-llvm 2>&1) | %check %s
// A closure whose type has no word for what it binds would write past
// itself: sammine-size-closures gives each type its words before this.

func.func private @add(%a: i32, %b: i32) -> i32 attributes {sammine.mir} {
  %s = arith.addi %a, %b : i32
  return %s : i32
}

// CHECK: error: '!sammine.fn<(i32) -> i32>' has no word for what this closure binds: sammine-size-closures sizes it
func.func @unsized(%x: i32) -> !sammine.fn<(i32) -> i32> {
  %one = sammine.closure @add(%x) : (i32) -> !sammine.fn<(i32) -> i32>
  return %one : !sammine.fn<(i32) -> i32>
}
