// RUN: %sammine_opt %s -split-input-file -verify-diagnostics

// A string literal is a view: nothing owns it, so nothing frees it.
func.func @owned() {
  // expected-error @+1 {{'sammine.string' op result #0 must be view of char}}
  %s = sammine.string "hi" : !sammine.ptr<i8, owned>
  return
}

// -----

func.func @not_char() {
  // expected-error @+1 {{'sammine.string' op result #0 must be view of char}}
  %s = sammine.string "hi" : !sammine.ptr<i32, view>
  return
}
