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

// Bound in a loop's body, dropped at the end of each turn.
// CHECK-LABEL: func.func @each_turn(
// CHECK: ^bb2:
// CHECK: %[[T:[0-9]+]] = sammine.let "t"
// CHECK-NEXT: sammine.drop %[[T]] implicit
// CHECK-NEXT: cf.br ^bb1
func.func @each_turn(%n: i32) attributes {sammine.mir} {
  %c = arith.constant true
  cf.br ^bb1
^bb1:
  cf.cond_br %c, ^bb2, ^bb3
^bb2:
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %t = sammine.let "t" %0 : !sammine.ptr<i32, owned>
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

// A box drops what it points at first, then is freed; once what it points
// at is moved out, it is only freed.
// CHECK-LABEL: func.func @box_of_owner(
// CHECK-NEXT: %[[PP:[0-9]+]] = sammine.let "pp" %arg0
// CHECK-NEXT: %[[AT:[0-9]+]] = sammine.deref %[[PP]]
// CHECK-NEXT: %[[IN:[0-9]+]] = sammine.move %[[AT]]
// CHECK-NEXT: sammine.drop %[[IN]] implicit : !sammine.ptr<i32, owned>
// CHECK-NEXT: sammine.drop %[[PP]] implicit
// CHECK-NEXT: return
// CHECK-LABEL: func.func @box_emptied(
// CHECK: sammine.drop %{{[0-9]+}} implicit : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
func.func @box_of_owner(%arg: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) attributes {sammine.mir} {
  %pp = sammine.let "pp" %arg : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}
func.func @box_emptied(%arg: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) attributes {sammine.mir} {
  %pp = sammine.let "pp" %arg : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  %place = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  %p = sammine.move %place : <!sammine.ptr<i32, owned>>
  sammine.drop %p : !sammine.ptr<i32, owned>
  return
}

// -----

// A struct without a Drop instance drops field by field, and only what is
// still there.
// CHECK-LABEL: func.func @fields(
// CHECK: %[[B:[0-9]+]] = sammine.extract %{{[0-9]+}}[1]
// CHECK-NEXT: sammine.drop %[[B]] implicit
// CHECK-NOT: sammine.drop
// CHECK: return
func.func private @take(!sammine.ptr<i32, owned>)
func.func @fields(%arg: !sammine.struct<"Two", (a: !sammine.ptr<i32, owned>, b: !sammine.ptr<i32, owned>)>) attributes {sammine.mir} {
  %s = sammine.let "s" %arg : !sammine.struct<"Two", (a: !sammine.ptr<i32, owned>, b: !sammine.ptr<i32, owned>)>
  %a = sammine.extract %s[0] : !sammine.struct<"Two", (a: !sammine.ptr<i32, owned>, b: !sammine.ptr<i32, owned>)> -> !sammine.ptr<i32, owned>
  sammine.call @take(%a) : (!sammine.ptr<i32, owned>) -> ()
  return
}

// -----

// One with a Drop instance drops whole, through it, but not in its own
// Drop, which would call itself: there, field by field.
// CHECK-LABEL: func.func @"Drop<Res>::Drop"(
// CHECK: sammine.extract %{{[0-9]+}}[0]
// CHECK: sammine.drop %{{[0-9]+}} implicit : !sammine.ptr<i32, owned>
// CHECK-LABEL: func.func @whole(
// CHECK-NEXT: %[[R:[0-9]+]] = sammine.let "r"
// CHECK-NEXT: sammine.drop %[[R]] implicit : !sammine.struct<"Res"
func.func @"Drop<Res>::Drop"(%arg: !sammine.struct<"Res", (p: !sammine.ptr<i32, owned>)>) attributes {sammine.mir} {
  %r = sammine.let "r" %arg : !sammine.struct<"Res", (p: !sammine.ptr<i32, owned>)>
  return
}
func.func @whole(%arg: !sammine.struct<"Res", (p: !sammine.ptr<i32, owned>)>) attributes {sammine.mir} {
  %r = sammine.let "r" %arg : !sammine.struct<"Res", (p: !sammine.ptr<i32, owned>)>
  return
}

// -----

// A store drops what the place held; the program's drop of a box drops
// what it points at first.
// CHECK-LABEL: func.func @before_store(
// CHECK: %[[OLD:[0-9]+]] = sammine.move %{{[0-9]+}}
// CHECK-NEXT: sammine.drop %[[OLD]] implicit
// CHECK-NEXT: sammine.store
// CHECK-LABEL: func.func @before_box_drop(
// CHECK: %[[IN:[0-9]+]] = sammine.move
// CHECK-NEXT: sammine.drop %[[IN]] implicit : !sammine.ptr<i32, owned>
// CHECK-NEXT: sammine.drop %arg0 : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
func.func @before_store(%pp: !sammine.ptr<!sammine.ptr<i32, owned>, owned>, %n: i32) -> !sammine.ptr<!sammine.ptr<i32, owned>, owned> attributes {sammine.mir} {
  %new = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %at = sammine.deref %pp : <!sammine.ptr<i32, owned>, owned>
  sammine.store %new, %at : <!sammine.ptr<i32, owned>>
  return %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
}
func.func @before_box_drop(%pp: !sammine.ptr<!sammine.ptr<i32, owned>, owned>) attributes {sammine.mir} {
  sammine.drop %pp : !sammine.ptr<!sammine.ptr<i32, owned>, owned>
  return
}

// -----

// An array drops whole: its elements move only with it.
// CHECK-LABEL: func.func @array(
// CHECK-NEXT: %[[A:[0-9]+]] = sammine.let "a"
// CHECK-NEXT: sammine.drop %[[A]] implicit : !sammine.array<2, !sammine.ptr<i32, owned>>
func.func @array(%arg: !sammine.array<2, !sammine.ptr<i32, owned>>) attributes {sammine.mir} {
  %a = sammine.let "a" %arg : !sammine.array<2, !sammine.ptr<i32, owned>>
  return
}

// -----

// A temporary nothing took has no owner: it isn't dropped, and the check
// reports it.
// CHECK-LABEL: func.func @temporary(
// CHECK-NOT: sammine.drop
// CHECK: return
func.func @temporary(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  return
}
