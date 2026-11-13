// RUN: %sammine_opt %s -sammine-mir-to-llvm | %check %s
// MIR's memory ops become what the AST codegen emits: malloc of the size
// times the count, widened to i64; free; a load or store at the pointer. A
// place and a view lent of it are both the pointer itself.

// CHECK: llvm.func @free(!llvm.ptr)
// CHECK: llvm.func @malloc(i64) -> !llvm.ptr
// CHECK-LABEL: func.func @f(%arg0: i32, %arg1: !llvm.ptr) -> i32
func.func @f(%n: i32, %pp: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) -> i32 {
  // CHECK: arith.extsi %arg0 : i32 to i64
  // CHECK: arith.constant 4 : i64
  // CHECK: arith.muli
  // CHECK: llvm.call @malloc(
  %p = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %place = sammine.deref %p : !sammine.ptr<i32, owned>
  %c = arith.constant 7 : i32
  // CHECK: llvm.store %{{.*}}, %{{.*}} : i32, !llvm.ptr
  sammine.store %c, %place : !sammine.place<i32>
  // CHECK: llvm.load %{{.*}} : !llvm.ptr -> i32
  %v = sammine.copy %place : !sammine.place<i32>
  // CHECK: llvm.call @free(
  sammine.drop %p : !sammine.ptr<i32, owned>
  %inner = sammine.deref %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // CHECK: llvm.load %arg1 : !llvm.ptr -> !llvm.ptr
  %q = sammine.move %inner : !sammine.place<!sammine.ptr<i32, owned>>
  sammine.drop %q : !sammine.ptr<i32, owned>
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  // CHECK-NOT: sammine.
  return %v : i32
}

func.func private @look(%p: !sammine.ptr<i32, view>) -> i32

// CHECK-LABEL: func.func @lend(%arg0: !llvm.ptr) -> i32
// CHECK-NEXT: %0 = call @look(%arg0) : (!llvm.ptr) -> i32
func.func @lend(%p: !sammine.ptr<i32, owned>) -> i32 {
  %place = sammine.deref %p : !sammine.ptr<i32, owned>
  %view = sammine.lend %place : !sammine.place<i32>
  %v = sammine.call @look(%view) : (!sammine.ptr<i32, view>) -> i32
  return %v : i32
}

// An element past a pointer is a GEP over its pointee's type.
// CHECK-LABEL: func.func @elem(%arg0: !llvm.ptr, %arg1: i64) -> i32
// CHECK-NEXT: %0 = llvm.getelementptr inbounds %arg0[%arg1] : (!llvm.ptr, i64) -> !llvm.ptr, i32
// CHECK-NEXT: %1 = llvm.load %0 : !llvm.ptr -> i32
func.func @elem(%p: !sammine.ptr<i32, view>, %i: i64) -> i32 {
  %pointee = sammine.deref %p : !sammine.ptr<i32, view>
  %place = sammine.ptr_elem %pointee[%i] : !sammine.place<i32>, i64
  %v = sammine.copy %place : !sammine.place<i32>
  return %v : i32
}

// What a pointer held in a place points at: the pointer, loaded.
// CHECK-LABEL: func.func @through(%arg0: !llvm.ptr) -> i32
// CHECK-NEXT: %0 = llvm.load %arg0 : !llvm.ptr -> !llvm.ptr
// CHECK-NEXT: %1 = llvm.load %0 : !llvm.ptr -> i32
func.func @through(%pp: !sammine.ptr<!sammine.ptr<i32, owned>, view>) -> i32 {
  %held = sammine.deref %pp : !sammine.ptr<!sammine.ptr<i32, owned>, view>
  %place = sammine.through %held : !sammine.place<!sammine.ptr<i32, owned>> -> !sammine.place<i32>
  %v = sammine.copy %place : !sammine.place<i32>
  return %v : i32
}
