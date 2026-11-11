#pragma once

#include "mlir/IR/Types.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"

//! \file CABI.h
//! \brief How the C calling convention (x86-64 System V) moves structs.
//!
//! LLVM leaves this to the frontend. A struct over 16 bytes goes through
//! memory. A smaller one is split into 8-byte chunks, each classified as
//! integer or floating point, and travels in registers as those chunks.
//! See https://yorickpeterse.com/articles/the-mess-that-is-handling-structure-arguments-and-returns-in-llvm/

namespace sammine_lang::cabi {

//! The type C holds `structTy` in when it travels in registers, e.g.
//! `{ i32, float, i64 }` as `{ i64, i64 }`, or nullptr when it goes through
//! memory.
mlir::Type registerType(mlir::Type structTy, const mlir::DataLayout &layout);

//! The registers C passes arguments in: 6 integer and 8 floating point.
inline constexpr unsigned kIntegerArgRegisters = 6;
inline constexpr unsigned kFloatArgRegisters = 8;

//! How many integer and floating-point registers an argument takes.
struct Registers {
  unsigned integer = 0;
  unsigned floating = 0;
};

//! The registers for an argument of `type`: a scalar, or a struct's register
//! type from registerType. A struct only goes in registers if all of them
//! are free; otherwise C passes it on the stack.
Registers registersFor(mlir::Type type);

} // namespace sammine_lang::cabi
