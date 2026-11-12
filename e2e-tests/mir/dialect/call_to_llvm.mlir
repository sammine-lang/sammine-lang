// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// sammine.call becomes func.call, its types LLVM's: they then match the
// callee's whether it went through MIR or was emitted from the AST.

func.func private @mir(%p: !sammine.ptr<i32, owned>) -> !sammine.ptr<i32, owned> attributes {sammine.mir} {
  return %p : !sammine.ptr<i32, owned>
}
func.func private @ast(%p: !llvm.ptr, %n: i64) -> i64

// CHECK-LABEL: func.func @f(%arg0: !llvm.ptr) -> i64
func.func @f(%p: !sammine.ptr<i32, owned>) -> i64 {
  // CHECK-NEXT: %0 = call @mir(%arg0) : (!llvm.ptr) -> !llvm.ptr
  %q = sammine.call @mir(%p) : (!sammine.ptr<i32, owned>) -> !sammine.ptr<i32, owned>
  %c = arith.constant 2 : i64
  // CHECK: %{{.*}} = call @ast(%0, %{{.*}}) : (!llvm.ptr, i64) -> i64
  %r = sammine.call @ast(%q, %c) : (!sammine.ptr<i32, owned>, i64) -> i64
  // CHECK-NOT: sammine.
  return %r : i64
}
