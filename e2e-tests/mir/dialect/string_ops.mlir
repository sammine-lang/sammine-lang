// RUN: %sammine_opt %s | %sammine_opt | %check %s
// A string literal prints as it parses, escapes and all.

// CHECK-LABEL: func.func @f(
func.func @f() -> !sammine.ptr<i8, view> {
  // CHECK: sammine.string "hi\0A\22there\22" : !sammine.ptr<i8, view>
  %s = sammine.string "hi\n\"there\"" : !sammine.ptr<i8, view>
  return %s : !sammine.ptr<i8, view>
}
