// RUN: %sammine_opt %s -split-input-file -sammine-check-ownership -verify-diagnostics
// The ownership check on MIR: a value that owns memory is used at most once
// on every path, and none is still owned where its function returns.

// Allocated, bound, freed: nothing to report.
func.func @freed(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  sammine.drop %p : !sammine.ptr<i32, owned>
  return
}

// -----

// Returning a value hands it on.
func.func @returned(%n: i32) -> !sammine.ptr<i32, owned> attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  return %p : !sammine.ptr<i32, owned>
}

// -----

// Only functions MIR defines are checked.
func.func @not_mir(%n: i32) {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  return
}

// -----

func.func @double_free(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  // expected-note @+1 {{'p' was dropped here}}
  sammine.drop %p : !sammine.ptr<i32, owned>
  // expected-error @+1 {{Cannot use linear variable 'p' — it was already dropped previously}}
  sammine.drop %p : !sammine.ptr<i32, owned>
  return
}

// -----

func.func @deref_after_free(%n: i32) -> i32 attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  // expected-note @+1 {{'p' was dropped here}}
  sammine.drop %p : !sammine.ptr<i32, owned>
  // expected-error @+1 {{Cannot dereference 'p' — it was already dropped previously}}
  %1 = sammine.deref %p : <i32, owned>
  %2 = sammine.copy %1 : <i32>
  return %2 : i32
}

// -----

func.func private @take(!sammine.ptr<i32, owned>, !sammine.ptr<i32, owned>)

func.func @twice_in_one_call(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  // expected-error @+2 {{Cannot use linear variable 'p' — it was already passed to function previously}}
  // expected-note @+1 {{'p' was passed to function here}}
  sammine.call @take(%p, %p) : (!sammine.ptr<i32, owned>, !sammine.ptr<i32, owned>) -> ()
  return
}

// -----

func.func @leak(%n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  // expected-error @+1 {{Linear variable 'p' must be consumed before scope exit (use drop() to deallocate)}}
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  return
}

// -----

func.func @discarded(%n: i32) attributes {sammine.mir} {
  // expected-error @+1 {{Temporary owning memory must not be discarded (bind it with 'let' first)}}
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  return
}

// -----

// Freed on every path.
func.func @both_branches(%c: i1, %n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  cf.cond_br %c, ^a, ^b
^a:
  sammine.drop %p : !sammine.ptr<i32, owned>
  cf.br ^done
^b:
  sammine.drop %p : !sammine.ptr<i32, owned>
  cf.br ^done
^done:
  return
}

// -----

// A branch that returns has no say in what is owned after the join.
func.func @branch_returns(%c: i1, %n: i32) -> i32 attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  cf.cond_br %c, ^a, ^b
^a:
  sammine.drop %p : !sammine.ptr<i32, owned>
  return %n : i32
^b:
  sammine.drop %p : !sammine.ptr<i32, owned>
  return %n : i32
}

// -----

func.func @one_branch(%c: i1, %n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  // expected-error @+1 {{Linear variable 'p' is consumed in some branches but not others}}
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  cf.cond_br %c, ^a, ^done
^a:
  // expected-note @+1 {{'p' was dropped here}}
  sammine.drop %p : !sammine.ptr<i32, owned>
  cf.br ^done
^done:
  return
}

// -----

func.func @use_after_one_branch(%c: i1, %n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  cf.cond_br %c, ^a, ^done
^a:
  // expected-note @+1 {{'p' was dropped here}}
  sammine.drop %p : !sammine.ptr<i32, owned>
  cf.br ^done
^done:
  // expected-error @+1 {{Cannot use linear variable 'p' — it was dropped in some branches}}
  sammine.drop %p : !sammine.ptr<i32, owned>
  return
}

// -----

// The check doesn't follow constants: `if false` may still run.
func.func @constant_condition(%n: i32) attributes {sammine.mir} {
  %false = arith.constant false
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  // expected-error @+1 {{Linear variable 'p' is consumed in some branches but not others}}
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  cf.cond_br %false, ^a, ^done
^a:
  // expected-note @+1 {{'p' was dropped here}}
  sammine.drop %p : !sammine.ptr<i32, owned>
  cf.br ^done
^done:
  return
}

// -----

// A value the branches pass to the join is owned by the join's argument.
func.func @joined(%c: i1, %n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %1 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %a = sammine.let "a" %0 : !sammine.ptr<i32, owned>
  %b = sammine.let "b" %1 : !sammine.ptr<i32, owned>
  cf.cond_br %c, ^left, ^right
^left:
  sammine.drop %b : !sammine.ptr<i32, owned>
  cf.br ^done(%a : !sammine.ptr<i32, owned>)
^right:
  sammine.drop %a : !sammine.ptr<i32, owned>
  cf.br ^done(%b : !sammine.ptr<i32, owned>)
^done(%r: !sammine.ptr<i32, owned>):
  %x = sammine.let "x" %r : !sammine.ptr<i32, owned>
  sammine.drop %x : !sammine.ptr<i32, owned>
  return
}

// -----

func.func private @consume(!sammine.ptr<i32, owned>)

// The loop's next turn would use what this one moved.
func.func @moved_in_loop(%c: i1, %n: i32) attributes {sammine.mir} {
  %0 = sammine.alloc %n : i32 -> !sammine.ptr<i32, owned>
  %p = sammine.let "p" %0 : !sammine.ptr<i32, owned>
  cf.br ^head
^head:
  cf.cond_br %c, ^body, ^exit
^body:
  // expected-error @+1 {{Cannot use linear variable 'p' — it was passed to function in an earlier turn of the loop}}
  sammine.call @consume(%p) : (!sammine.ptr<i32, owned>) -> ()
  cf.br ^head
^exit:
  return
}
