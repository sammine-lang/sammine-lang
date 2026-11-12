// RUN: %sammine_opt %s -split-input-file -verify-diagnostics

// MLIR's generated parser reports a bad enum keyword, then the parameter.
// expected-error @+2 {{expected ::sammine_lang::smn::Ownership to be one of: owned, view}}
// expected-error @+1 {{failed to parse Sammine_PtrType parameter 'ownership'}}
func.func private @bad(!sammine.ptr<i32, borrowed>)

// -----

// expected-error @+1 {{expected ','}}
func.func private @no_ownership(!sammine.ptr<i32>)
