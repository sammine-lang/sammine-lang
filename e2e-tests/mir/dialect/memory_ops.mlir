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
  // CHECK: sammine.drop %0 : !sammine.ptr<i32, owned>
  sammine.drop %p : !sammine.ptr<i32, owned>
  %inner = sammine.deref %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // CHECK: sammine.move %{{.*}} : <!sammine.ptr<i32, owned>>
  %q = sammine.move %inner : !sammine.place<!sammine.ptr<i32, owned>>
  sammine.drop %q : !sammine.ptr<i32, owned>
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return %v : i32
}

func.func private @look(%p: !sammine.ptr<!sammine.ptr<i32, owned>, view>)

// CHECK-LABEL: func.func @lend(
func.func @lend(%p: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) {
  %place = sammine.deref %p : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // CHECK: %1 = sammine.lend %0 : <!sammine.ptr<i32, owned>>
  %view = sammine.lend %place : !sammine.place<!sammine.ptr<i32, owned>>
  // CHECK-NEXT: sammine.call @look(%1)
  sammine.call @look(%view) : (!sammine.ptr<!sammine.ptr<i32, owned>, view>) -> ()
  return
}

// CHECK-LABEL: func.func @buffer(
func.func @buffer(%n: i64) {
  // CHECK: sammine.alloc %arg0 : i64 -> !sammine.ptr<i32, buffer>
  %b = sammine.alloc %n : i64 -> !sammine.ptr<i32, buffer>
  // CHECK: sammine.drop %0 : !sammine.ptr<i32, buffer>
  sammine.drop %b : !sammine.ptr<i32, buffer>
  return
}
