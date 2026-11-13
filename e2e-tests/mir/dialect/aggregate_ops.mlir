// RUN: %sammine_opt %s | %sammine_opt | %check %s
// Struct and tuple types, and building and reading them, print as they
// parse.

// CHECK-LABEL: func.func @f(
func.func @f(%a: i32, %p: !sammine.ptr<i32, owned>) -> !sammine.ptr<i32, owned> {
  // CHECK: sammine.aggregate(%arg0, %arg1) : (i32, !sammine.ptr<i32, owned>) -> !sammine.struct<"Box", (n: i32, p: !sammine.ptr<i32, owned>)>
  %s = sammine.aggregate(%a, %p) : (i32, !sammine.ptr<i32, owned>) -> !sammine.struct<"Box", (n: i32, p: !sammine.ptr<i32, owned>)>
  // CHECK: sammine.extract %{{.*}}[1] : !sammine.struct<"Box", (n: i32, p: !sammine.ptr<i32, owned>)> -> !sammine.ptr<i32, owned>
  %q = sammine.extract %s[1] : !sammine.struct<"Box", (n: i32, p: !sammine.ptr<i32, owned>)> -> !sammine.ptr<i32, owned>
  // CHECK: sammine.aggregate(%arg0, %arg0) : (i32, i32) -> !sammine.tuple<i32, i32>
  %t = sammine.aggregate(%a, %a) : (i32, i32) -> !sammine.tuple<i32, i32>
  return %q : !sammine.ptr<i32, owned>
}
