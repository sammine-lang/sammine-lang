// RUN: %sammine_opt %s | %sammine_opt | %check %s
// A binding's slot, places within it, and a view of one print as they parse.

// CHECK-LABEL: func.func @f(
func.func @f(%t: !sammine.struct<"P", (a: i32, b: !sammine.array<2, i64>)>, %i: i32) -> i64 {
  // CHECK: sammine.local "t" : <!sammine.struct<"P", (a: i32, b: !sammine.array<2, i64>)>>
  %slot = sammine.local "t" : !sammine.place<!sammine.struct<"P", (a: i32, b: !sammine.array<2, i64>)>>
  sammine.store %t, %slot : !sammine.place<!sammine.struct<"P", (a: i32, b: !sammine.array<2, i64>)>>
  // CHECK: sammine.field %0[1] : <!sammine.struct<"P", (a: i32, b: !sammine.array<2, i64>)>> -> <!sammine.array<2, i64>>
  %arr = sammine.field %slot[1] : !sammine.place<!sammine.struct<"P", (a: i32, b: !sammine.array<2, i64>)>> -> !sammine.place<!sammine.array<2, i64>>
  // CHECK: sammine.elem %1[%arg1] : <!sammine.array<2, i64>>, i32 -> <i64>
  %e = sammine.elem %arr[%i] : !sammine.place<!sammine.array<2, i64>>, i32 -> !sammine.place<i64>
  // CHECK: sammine.addr %2 : <i64>
  %view = sammine.addr %e : !sammine.place<i64>
  %v = sammine.copy %e : !sammine.place<i64>
  return %v : i64
}
