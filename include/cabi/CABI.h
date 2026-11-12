#pragma once

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Types.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"

//! \file CABI.h
//! \brief How the C calling convention (x86-64 System V) moves structs, how
//! a call to C is made, and how C lays out a string.
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

//! A call to the C function `callee` with `args`, already in the types its
//! declaration takes (a struct in its register type or as a byval pointer,
//! an sret buffer first). An argument passed through `...` is promoted as C
//! does by default: a float to a double, a char (signed) and a bool
//! (unsigned) to an int. The call carries the declaration's argument
//! attributes (sret, byval, signext, zeroext), which LLVM reads from the
//! call.
//! A constant global named `name`, at the top of `module`, holding `text` as
//! C reads a string: its bytes, then a NUL.
mlir::LLVM::GlobalOp createString(mlir::OpBuilder &builder,
                                  mlir::ModuleOp module, mlir::Location loc,
                                  llvm::StringRef name, llvm::StringRef text);

mlir::LLVM::CallOp emitCall(mlir::OpBuilder &builder, mlir::Location loc,
                            mlir::LLVM::LLVMFuncOp callee,
                            llvm::SmallVector<mlir::Value, 4> args);

//! A call to the C function `callee` with `args` as sammine has them. A
//! struct argument goes as C passes it, in its register type or as a
//! pointer to a copy (byval); a struct result comes back in its register
//! type or through a buffer passed first (sret), and reaches `result`,
//! sammine's type for it, through memory. Returns the result, or a null
//! value if `result` is null. `slot` makes a stack slot of a type.
mlir::Value callC(mlir::OpBuilder &builder, mlir::Location loc,
                  mlir::LLVM::LLVMFuncOp callee,
                  llvm::SmallVector<mlir::Value, 4> args, mlir::Type result,
                  llvm::function_ref<mlir::Value(mlir::Type)> slot);

} // namespace sammine_lang::cabi
