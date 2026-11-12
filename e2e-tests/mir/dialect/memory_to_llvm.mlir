// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// MIR's memory ops become what the AST codegen emits: malloc of the size
// times the count, widened to i64; free; a load or store at the pointer. A
// place and a view lent of it are both the pointer itself.

// CHECK: llvm.func @free(!llvm.ptr)
// CHECK: llvm.func @malloc(i64) -> !llvm.ptr
// CHECK-LABEL: func.func @f(%arg0: i32, %arg1: !llvm.ptr) -> i32
func.func @f(%n: i32, %pp: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) -> i32 {
  // CHECK: arith.extsi %arg0 : i32 to i64
  // CHECK: arith.constant 4 : i64
  // CHECK: arith.muli
  // CHECK: llvm.call @malloc(
  %p = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %place = sammine.deref %p : !sammine.ptr<i32, owned>
  %c = arith.constant 7 : i32
  // CHECK: llvm.store %{{.*}}, %{{.*}} : i32, !llvm.ptr
  sammine.store %c, %place : !sammine.place<i32>
  // CHECK: llvm.load %{{.*}} : !llvm.ptr -> i32
  %v = sammine.copy %place : !sammine.place<i32>
  // CHECK: llvm.call @free(
  sammine.free %p : !sammine.ptr<i32, owned>
  %inner = sammine.deref %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // CHECK: llvm.load %arg1 : !llvm.ptr -> !llvm.ptr
  %q = sammine.move %inner : !sammine.place<!sammine.ptr<i32, owned>>
  sammine.free %q : !sammine.ptr<i32, owned>
  sammine.free %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // CHECK-NOT: sammine.
  return %v : i32
}

func.func private @look(%p: !sammine.ptr<i32, view>) -> i32

// CHECK-LABEL: func.func @lend(%arg0: !llvm.ptr) -> i32
// CHECK-NEXT: %0 = call @look(%arg0) : (!llvm.ptr) -> i32
func.func @lend(%p: !sammine.ptr<i32, owned>) -> i32 {
  %place = sammine.deref %p : !sammine.ptr<i32, owned>
  %view = sammine.lend %place : !sammine.place<i32>
  %v = sammine.call @look(%view) : (!sammine.ptr<i32, view>) -> i32
  return %v : i32
}
