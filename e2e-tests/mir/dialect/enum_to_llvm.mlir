// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// An enum is the identified struct sammine.enum.<name>, an i32 tag then the
// payload's bytes, as the AST codegen makes it. A variant is built in a stack
// slot at the top of the function: the tag, each payload field at its
// offset, then loaded whole. The tag is the struct's first field; a payload
// field is read back through memory from its offset.

// CHECK-LABEL: func.func @f(%arg0: i8, %arg1: i64) -> i64
func.func @f(%c: i8, %x: i64) -> i64 {
  // CHECK: llvm.alloca %{{.*}} x !llvm.struct<"sammine.enum.Pair", (i32, array<16 x i8>)>
  // CHECK: llvm.store %c1_i32,
  // CHECK: llvm.getelementptr inbounds %{{.*}}[0, 1]
  // CHECK: llvm.store %arg0,
  // CHECK: llvm.getelementptr inbounds %{{.*}}[8] : (!llvm.ptr) -> !llvm.ptr, i8
  // CHECK: llvm.store %arg1,
  // CHECK: llvm.load %{{.*}} : !llvm.ptr -> !llvm.struct<"sammine.enum.Pair", (i32, array<16 x i8>)>
  %e = sammine.variant 1(%c, %x) at [0, 8] : (i8, i64) -> !sammine.enum<"Pair", 16, (Empty: !sammine.tuple<>, Both: !sammine.tuple<i8, i64>)>
  // CHECK: llvm.extractvalue %{{.*}}[0] : !llvm.struct<"sammine.enum.Pair", (i32, array<16 x i8>)>
  %t = sammine.tag %e : !sammine.enum<"Pair", 16, (Empty: !sammine.tuple<>, Both: !sammine.tuple<i8, i64>)>
  // CHECK: llvm.load %{{.*}} : !llvm.ptr -> i64
  %v = sammine.payload %e[1, 1] at 8 : !sammine.enum<"Pair", 16, (Empty: !sammine.tuple<>, Both: !sammine.tuple<i8, i64>)> -> i64
  // CHECK-NOT: sammine.
  return %v : i64
}
