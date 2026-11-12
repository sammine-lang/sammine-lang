// RUN: %sammine_opt %s | %sammine_opt | %check %s
// sammine.call prints as it parses, to a callee through MIR or one emitted
// from the AST, whose LLVM types differ from MIR's.

func.func private @mir(%p: !sammine.ptr<i32, owned>, %n: i32) -> i32 attributes {sammine.mir} {
  return %n : i32
}
func.func private @ast(%p: !llvm.ptr)
func.func private @nothing() attributes {sammine.mir} {
  return
}

// CHECK-LABEL: func.func @f(
func.func @f(%p: !sammine.ptr<i32, owned>) -> i32 {
  %c = arith.constant 1 : i32
  // CHECK: %{{.*}} = sammine.call @mir(%arg0, %{{.*}}) : (!sammine.ptr<i32, owned>, i32) -> i32
  %r = sammine.call @mir(%p, %c) : (!sammine.ptr<i32, owned>, i32) -> i32
  // CHECK: sammine.call @ast(%arg0) : (!sammine.ptr<i32, owned>) -> ()
  sammine.call @ast(%p) : (!sammine.ptr<i32, owned>) -> ()
  // CHECK: sammine.call @nothing() : () -> ()
  sammine.call @nothing() : () -> ()
  return %r : i32
}
