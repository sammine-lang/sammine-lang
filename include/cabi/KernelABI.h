#pragma once

#include "mlir/IR/Builders.h"

//! \file KernelABI.h
//! \brief How a kernel's wrapper, which the kernel codegen makes, takes an
//! array: by pointer on the CPU, and on the GPU as a memref, built here from
//! the array's address.

namespace sammine_lang::kernel_abi {

//! A one-dimensional memref of `size` elements of `element` at `ptr`: its
//! LLVM descriptor, cast to the memref type, which lowering to LLVM turns
//! back into the descriptor.
mlir::Value memrefOfPointer(mlir::OpBuilder &builder, mlir::Location loc,
                            mlir::Value ptr, int64_t size,
                            mlir::Type element);

} // namespace sammine_lang::kernel_abi
