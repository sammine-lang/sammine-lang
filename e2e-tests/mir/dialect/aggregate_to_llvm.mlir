// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// A struct is the identified LLVM struct sammine.struct.<name>, the AST
// codegen's type for it; a tuple a literal LLVM struct. Building one inserts
// each field, reading one extracts it.

// CHECK-LABEL: func.func @f(%arg0: i32, %arg1: !llvm.ptr) -> !llvm.struct<(i32, ptr)>
func.func @f(%a: i32, %p: !sammine.ptr<i32, owned>) -> !sammine.tuple<i32, !sammine.ptr<i32, owned>> {
  // CHECK-NEXT: %0 = llvm.mlir.poison : !llvm.struct<"sammine.struct.Box", (i32, ptr)>
  // CHECK-NEXT: %1 = llvm.insertvalue %arg0, %0[0] : !llvm.struct<"sammine.struct.Box", (i32, ptr)>
  // CHECK-NEXT: %2 = llvm.insertvalue %arg1, %1[1] : !llvm.struct<"sammine.struct.Box", (i32, ptr)>
  %s = sammine.aggregate(%a, %p) : (i32, !sammine.ptr<i32, owned>) -> !sammine.struct<"Box", (n: i32, p: !sammine.ptr<i32, owned>)>
  // CHECK-NEXT: %3 = llvm.extractvalue %2[1] : !llvm.struct<"sammine.struct.Box", (i32, ptr)>
  %q = sammine.extract %s[1] : !sammine.struct<"Box", (n: i32, p: !sammine.ptr<i32, owned>)> -> !sammine.ptr<i32, owned>
  // CHECK: llvm.insertvalue %3, %{{.*}}[1] : !llvm.struct<(i32, ptr)>
  %t = sammine.aggregate(%a, %q) : (i32, !sammine.ptr<i32, owned>) -> !sammine.tuple<i32, !sammine.ptr<i32, owned>>
  return %t : !sammine.tuple<i32, !sammine.ptr<i32, owned>>
}
