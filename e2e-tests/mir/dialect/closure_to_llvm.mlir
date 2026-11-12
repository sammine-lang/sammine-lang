// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// A function value is the AST codegen's closure, {code, environment}. A
// function's own value shares the wrapper __wrap_<f>, with a null
// environment; a partial application keeps what it binds in a stack slot,
// read by a wrapper of its own. A call through a value passes the
// environment first.

func.func private @add(%a: i32, %b: i32) -> i32 attributes {sammine.mir} {
  %s = arith.addi %a, %b : i32
  return %s : i32
}

// CHECK-LABEL: func.func @f(%arg0: i32) -> i32
// CHECK: llvm.alloca %{{.*}} x !llvm.struct<(i32)>
// CHECK: llvm.mlir.zero : !llvm.ptr
// CHECK: llvm.mlir.addressof @__wrap_add
// CHECK: llvm.mlir.addressof @__partial_0
// CHECK: llvm.call %{{.*}}(%{{.*}}, %arg0) : !llvm.ptr, (!llvm.ptr, i32) -> i32
func.func @f(%x: i32) -> i32 {
  %all = sammine.closure @add() : () -> !sammine.fn<(i32, i32) -> i32>
  %one = sammine.closure @add(%x) : (i32) -> !sammine.fn<(i32) -> i32>
  %r = sammine.call_indirect %one(%x) : !sammine.fn<(i32) -> i32>, (i32) -> i32
  return %r : i32
}
// CHECK-LABEL: llvm.func internal @__wrap_add(%arg0: !llvm.ptr, %arg1: i32, %arg2: i32) -> i32
// CHECK: call @add(%arg1, %arg2)
// CHECK-LABEL: llvm.func internal @__partial_0(%arg0: !llvm.ptr, %arg1: i32) -> i32
// CHECK: llvm.load
// CHECK: call @add(
