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

// A block after a branch takes MIR types too: its arguments are converted
// with the function's.
// CHECK-LABEL: func.func private @choose(%arg0: !llvm.ptr, %arg1: !llvm.ptr, %arg2: i1) -> !llvm.ptr
// CHECK: bb3(%{{[0-9]+}}: !llvm.ptr):
func.func private @choose(%a: !sammine.ptr<i32, owned>, %b: !sammine.ptr<i32, owned>, %c: i1) -> !sammine.ptr<i32, owned> {
  cf.cond_br %c, ^bb1, ^bb2
^bb1:
  cf.br ^bb3(%a : !sammine.ptr<i32, owned>)
^bb2:
  cf.br ^bb3(%b : !sammine.ptr<i32, owned>)
^bb3(%r: !sammine.ptr<i32, owned>):
  return %r : !sammine.ptr<i32, owned>
}
