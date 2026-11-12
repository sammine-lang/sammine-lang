// RUN: %sammine_opt %s | %sammine_opt | %check %s
// !sammine.ptr prints as it parses, owned or a view, of any type, a pointer
// to a pointer included.

// CHECK-LABEL: func.func private @take(!sammine.ptr<i32, owned>) -> i32
func.func private @take(!sammine.ptr<i32, owned>) -> i32

// CHECK-LABEL: func.func private @look(!sammine.ptr<i32, view>)
func.func private @look(!sammine.ptr<i32, view>)

// CHECK-LABEL: func.func private @nested(!sammine.ptr<!sammine.ptr<f64, owned>, view>)
func.func private @nested(!sammine.ptr<!sammine.ptr<f64, owned>, view>)
