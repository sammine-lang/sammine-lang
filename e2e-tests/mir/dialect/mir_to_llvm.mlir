// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// !sammine.ptr becomes !llvm.ptr in signatures, calls and returns. A cast a
// caller made into MIR is left casting !llvm.ptr to itself, for reconciling
// to remove.

// CHECK-LABEL: func.func private @pass(%arg0: !llvm.ptr) -> !llvm.ptr
// CHECK-NEXT: return %arg0 : !llvm.ptr
func.func private @pass(%arg0: !sammine.ptr<i32, owned>) -> !sammine.ptr<i32, owned> {
  return %arg0 : !sammine.ptr<i32, owned>
}

// CHECK-LABEL: func.func @caller(%arg0: !llvm.ptr) -> !llvm.ptr
// CHECK: call @pass(%{{.*}}) : (!llvm.ptr) -> !llvm.ptr
// CHECK-NOT: !sammine.ptr<i32, owned>) ->
func.func @caller(%raw: !llvm.ptr) -> !llvm.ptr {
  %p = builtin.unrealized_conversion_cast %raw : !llvm.ptr to !sammine.ptr<i32, owned>
  %q = call @pass(%p) : (!sammine.ptr<i32, owned>) -> !sammine.ptr<i32, owned>
  %r = builtin.unrealized_conversion_cast %q : !sammine.ptr<i32, owned> to !llvm.ptr
  return %r : !llvm.ptr
}

// A function with nothing of MIR's is left as it is.
// CHECK-LABEL: func.func @plain(%arg0: i32) -> i32
// CHECK-NEXT: return %arg0 : i32
func.func @plain(%arg0: i32) -> i32 {
  return %arg0 : i32
}
