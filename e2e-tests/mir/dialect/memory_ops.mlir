// RUN: %sammine_opt %s | %sammine_opt | %check %s
// MIR's memory ops print as they parse.

// CHECK-LABEL: func.func @f(
func.func @f(%n: i64, %pp: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) -> i32 {
  // CHECK: sammine.alloc %arg0 : i64 -> !sammine.ptr<i32, owned>
  %p = sammine.alloc %n : i64 -> !sammine.ptr<i32, owned>
  // CHECK: sammine.deref %0 : <i32, owned>
  %place = sammine.deref %p : !sammine.ptr<i32, owned>
  %c = arith.constant 7 : i32
  // CHECK: sammine.store %{{.*}}, %1 : <i32>
  sammine.store %c, %place : !sammine.place<i32>
  // CHECK: sammine.copy %1 : <i32>
  %v = sammine.copy %place : !sammine.place<i32>
  // CHECK: sammine.free %0 : !sammine.ptr<i32, owned>
  sammine.free %p : !sammine.ptr<i32, owned>
  %inner = sammine.deref %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // CHECK: sammine.move %{{.*}} : <!sammine.ptr<i32, owned>>
  %q = sammine.move %inner : !sammine.place<!sammine.ptr<i32, owned>>
  sammine.free %q : !sammine.ptr<i32, owned>
  sammine.free %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return %v : i32
}
