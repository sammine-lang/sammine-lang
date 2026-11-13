// RUN: %sammine_opt %s | %sammine_opt | %check %s
// sammine.call prints as it parses, to a callee through MIR, one emitted
// from the AST, whose LLVM types differ from MIR's, or a C function, here
// variadic.

func.func private @mir(%p: !sammine.ptr<i32, owned>, %n: i32) -> i32 attributes {sammine.mir} {
  return %n : i32
}
func.func private @ast(%p: !llvm.ptr)
func.func private @nothing() attributes {sammine.mir} {
  return
}
llvm.func @printf(!llvm.ptr, ...) -> i32

// CHECK-LABEL: func.func @f(
func.func @f(%p: !sammine.ptr<i32, owned>, %fmt: !sammine.ptr<i8, view>) -> i32 {
  %c = arith.constant 1 : i32
  // CHECK: %{{.*}} = sammine.call @mir(%arg0, %{{.*}}) : (!sammine.ptr<i32, owned>, i32) -> i32
  %r = sammine.call @mir(%p, %c) : (!sammine.ptr<i32, owned>, i32) -> i32
  // CHECK: sammine.call @ast(%arg0) : (!sammine.ptr<i32, owned>) -> ()
  sammine.call @ast(%p) : (!sammine.ptr<i32, owned>) -> ()
  // CHECK: sammine.call @nothing() : () -> ()
  sammine.call @nothing() : () -> ()
  %ch = arith.constant 97 : i8
  // CHECK: sammine.call @printf(%arg1, %{{.*}}, %{{.*}}) : (!sammine.ptr<i8, view>, i32, i8) -> i32
  %n = sammine.call @printf(%fmt, %r, %ch) : (!sammine.ptr<i8, view>, i32, i8) -> i32
  return %r : i32
}

// -----

// A call standing for other syntax says what was written.
func.func private @"Convert<A, B>::Convert"(i32) -> i64
// CHECK-LABEL: func.func @cast
func.func @cast(%x: i32) -> i64 {
  // CHECK: sammine.call @"Convert<A, B>::Convert"(%arg0) written "as" : (i32) -> i64
  %y = sammine.call @"Convert<A, B>::Convert"(%x) written "as" : (i32) -> i64
  return %y : i64
}
