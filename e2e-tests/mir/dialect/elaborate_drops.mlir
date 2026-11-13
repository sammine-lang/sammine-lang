// RUN: %sammine_opt %s -split-input-file -sammine-elaborate-drops | %check %s
// Each value still owned where it goes out of use gets an implicit drop:
// at a return, or on the edge where it stops being live.

// CHECK-LABEL: func.func @at_return(
// CHECK: %[[P:[0-9]+]] = sammine.let "p"
// CHECK-NEXT: sammine.drop %[[P]] implicit : !sammine.ptr<i32, owned>
// CHECK-NEXT: return
func.func @at_return(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  return
}

// -----

// Bound in an arm, dropped on the arm's way to the join; nothing at the
// return, which it doesn't reach.
// CHECK-LABEL: func.func @end_of_arm(
// CHECK: ^bb1:
// CHECK: %[[Q:[0-9]+]] = sammine.let "q"
// CHECK-NEXT: sammine.drop %[[Q]] implicit
// CHECK-NEXT: cf.br ^bb2
// CHECK: ^bb2:
// CHECK-NEXT: return
func.func @end_of_arm(%c: i1, %n: i32) attributes {sammine.mir} {
  cf.cond_br %c, ^bb1, ^bb2
^bb1:
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %q = sammine.let "q" %0 : !sammine.ptr<i32, owned>
  cf.br ^bb2
^bb2:
  return
}

// -----

// Dropped by the program on one side, so dropped on the edge of the other,
// in a block of its own: no flag at run time.
// CHECK-LABEL: func.func @moved_on_one_side(
// CHECK: %[[P:[0-9]+]] = sammine.let "p"
// CHECK-NEXT: cf.cond_br %arg0, ^[[DROPS:bb[0-9]+]], ^[[EDGE:bb[0-9]+]]
// CHECK: ^[[EDGE]]:
// CHECK-NEXT: sammine.drop %[[P]] implicit
// CHECK-NEXT: cf.br ^[[JOIN:bb[0-9]+]]
// CHECK: ^[[DROPS]]:
// CHECK-NEXT: sammine.drop %[[P]] :
// CHECK-NEXT: cf.br ^[[JOIN]]
func.func @moved_on_one_side(%c: i1, %n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  cf.cond_br %c, ^bb1, ^bb2
^bb1:
  sammine.drop %p : !sammine.ptr<i32, owned>
  cf.br ^bb2
^bb2:
  return
}

// -----

// Made in a loop's body, dropped at the end of each turn.
// CHECK-LABEL: func.func @each_turn(
// CHECK: ^bb2:
// CHECK: %[[T:[0-9]+]] = sammine.alloc
// CHECK-NEXT: sammine.drop %[[T]] implicit
// CHECK-NEXT: cf.br ^bb1
func.func @each_turn(%n: i32) attributes {sammine.mir} {
  %c = arith.constant true
  cf.br ^bb1
^bb1:
  cf.cond_br %c, ^bb2, ^bb3
^bb2:
  %t = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  cf.br ^bb1
^bb3:
  return
}

// -----

// A slot is moved out of, then dropped.
// CHECK-LABEL: func.func @slot(
// CHECK: %[[V:[0-9]+]] = sammine.move %{{[0-9]+}} : <!sammine.ptr<i32, owned>>
// CHECK-NEXT: sammine.drop %[[V]] implicit
func.func @slot(%n: i32) attributes {sammine.mir} {
  %slot = sammine.local "a" : <!sammine.ptr<i32, owned>>
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  sammine.store %0, %slot : <!sammine.ptr<i32, owned>>
  return
}

// -----

// A box whose pointee owns memory isn't dropped yet: its drop isn't a
// free. Once what it points at is moved out, it is.
// CHECK-LABEL: func.func @box_of_owner(
// CHECK-NOT: implicit
// CHECK: return
// CHECK-LABEL: func.func @box_emptied(
// CHECK: sammine.drop %arg0 implicit : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
func.func @box_of_owner(%pp: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) attributes {sammine.mir} {
  return
}
func.func @box_emptied(%pp: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) attributes {sammine.mir} {
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  %p = sammine.move %place : <!sammine.ptr<i32, owned>>
  sammine.drop %p : !sammine.ptr<i32, owned>
  return
}
