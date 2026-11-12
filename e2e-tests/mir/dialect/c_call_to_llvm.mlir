// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// A call to a C function declared as an llvm.func becomes llvm.call, made as
// C makes it (cabi::emitCall): an argument passed through `...` is promoted,
// a char sign-extended and a bool zero-extended to i32 and an f32 widened to
// f64, and the call carries the declaration's argument attributes.

llvm.func @printf(!llvm.ptr {llvm.noundef}, ...) -> i32

// CHECK-LABEL: func.func @show(%arg0: !llvm.ptr, %arg1: i8, %arg2: i1, %arg3: f32, %arg4: i32) -> i32
func.func @show(%fmt: !sammine.ptr<i8, view>, %c: i8, %b: i1, %f: f32, %n: i32) -> i32 {
  // CHECK-NEXT: %0 = llvm.sext %arg1 : i8 to i32
  // CHECK-NEXT: %1 = llvm.zext %arg2 : i1 to i32
  // CHECK-NEXT: %2 = llvm.fpext %arg3 : f32 to f64
  // CHECK-NEXT: %3 = llvm.call @printf(%arg0, %0, %1, %2, %arg4) vararg(!llvm.func<i32 (ptr, ...)>) : (!llvm.ptr {llvm.noundef}, i32, i32, f64, i32) -> i32
  %r = sammine.call @printf(%fmt, %c, %b, %f, %n) : (!sammine.ptr<i8, view>, i8, i1, f32, i32) -> i32
  return %r : i32
}
