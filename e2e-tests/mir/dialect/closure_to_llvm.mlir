// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// A function value is {code, words}: the words carry what the closure
// binds, by value, so the value can go anywhere. A function's own value
// shares the wrapper __wrap_<f> and carries nothing; a partial application
// writes what it binds into the words, read by a wrapper of its own. A call
// through a value passes the code the words first, by value, so no address
// of the caller's stack reaches it. A type no closure binds anything for is
// the code alone, whose code is passed a null pointer.

func.func private @add(%a: i32, %b: i32) -> i32 attributes {sammine.mir} {
  %s = arith.addi %a, %b : i32
  return %s : i32
}

// CHECK-LABEL: func.func @f(%arg0: i32) -> i32
// CHECK: llvm.alloca %{{.*}} x !llvm.struct<(ptr, array<1 x i64>)>
// CHECK: llvm.mlir.addressof @__wrap_add
// CHECK: llvm.insertvalue %{{.*}}, %{{.*}}[0] : !llvm.struct<(ptr)>
// CHECK: llvm.mlir.addressof @__partial_0
// CHECK: llvm.store %arg0
// CHECK: llvm.load %{{.*}} : !llvm.ptr -> !llvm.struct<(ptr, array<1 x i64>)>
// CHECK: llvm.extractvalue %{{.*}}[1] : !llvm.struct<(ptr, array<1 x i64>)>
// CHECK: llvm.call %{{.*}}(%{{.*}}, %arg0) : !llvm.ptr, (!llvm.array<1 x i64>, i32) -> i32
func.func @f(%x: i32) -> i32 {
  %all = sammine.closure @add() : () -> !sammine.fn<(i32, i32) -> i32>
  %one = sammine.closure @add(%x) : (i32) -> !sammine.fn<(i32) -> i32, 1>
  %r = sammine.call_indirect %one(%x) : !sammine.fn<(i32) -> i32, 1>, (i32) -> i32
  return %r : i32
}
// CHECK-LABEL: llvm.func internal @__wrap_add(%arg0: !llvm.ptr, %arg1: i32, %arg2: i32) -> i32
// CHECK: call @add(%arg1, %arg2)
// CHECK-LABEL: llvm.func internal @__partial_0(%arg0: !llvm.array<1 x i64>, %arg1: i32) -> i32
// CHECK: llvm.store %arg0
// CHECK: llvm.load
// CHECK: call @add(
