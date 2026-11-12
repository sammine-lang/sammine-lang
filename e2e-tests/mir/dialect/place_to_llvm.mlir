// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// A slot is a stack slot at the top of the function, a field's or an
// element's place a GEP into it, and a view of a place its address.

// CHECK-LABEL: func.func @f(%arg0: !llvm.struct<(i32, array<2 x i64>)>, %arg1: i32) -> !llvm.ptr
// CHECK: llvm.alloca %{{.*}} x !llvm.struct<(i32, array<2 x i64>)>
// CHECK: llvm.store %arg0,
// CHECK: llvm.getelementptr inbounds %{{.*}}[0, 1] : (!llvm.ptr) -> !llvm.ptr, !llvm.struct<(i32, array<2 x i64>)>
// CHECK: llvm.getelementptr inbounds %{{.*}}[0, %arg1] : (!llvm.ptr, i32) -> !llvm.ptr, !llvm.array<2 x i64>
// CHECK-NOT: sammine.
func.func @f(%t: !sammine.tuple<i32, !sammine.array<2, i64>>, %i: i32) -> !sammine.ptr<i64, view> {
  %slot = sammine.local "t" : !sammine.place<!sammine.tuple<i32, !sammine.array<2, i64>>>
  sammine.store %t, %slot : !sammine.place<!sammine.tuple<i32, !sammine.array<2, i64>>>
  %arr = sammine.field %slot[1] : !sammine.place<!sammine.tuple<i32, !sammine.array<2, i64>>> -> !sammine.place<!sammine.array<2, i64>>
  %e = sammine.elem %arr[%i] : !sammine.place<!sammine.array<2, i64>>, i32 -> !sammine.place<i64>
  %view = sammine.addr %e : !sammine.place<i64>
  return %view : !sammine.ptr<i64, view>
}
