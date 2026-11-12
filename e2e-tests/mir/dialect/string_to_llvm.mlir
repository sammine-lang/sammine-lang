// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// A string literal becomes the address of a constant global holding its
// bytes and a NUL, one global for each distinct string.

// Each global goes at the top of the module, so the later one comes first.
// CHECK: llvm.mlir.global internal constant @".mir.str.1"("bye\00")
// CHECK-NEXT: llvm.mlir.global internal constant @".mir.str.0"("hi\00")
// CHECK-LABEL: func.func @f() -> !llvm.ptr
func.func @f() -> !sammine.ptr<i8, view> {
  // CHECK-NEXT: llvm.mlir.addressof @".mir.str.0" : !llvm.ptr
  %a = sammine.string "hi" : !sammine.ptr<i8, view>
  // CHECK-NEXT: llvm.mlir.addressof @".mir.str.1" : !llvm.ptr
  %b = sammine.string "bye" : !sammine.ptr<i8, view>
  // CHECK-NEXT: llvm.mlir.addressof @".mir.str.0" : !llvm.ptr
  %c = sammine.string "hi" : !sammine.ptr<i8, view>
  return %c : !sammine.ptr<i8, view>
}
