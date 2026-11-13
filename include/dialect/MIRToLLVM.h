#pragma once

#include "mlir/IR/Types.h"
#include "mlir/Pass/Pass.h"

#include <memory>

//! \file MIRToLLVM.h
//! \brief Converts MIR's types and ops to the LLVM dialect, before the rest of
//! the module is lowered to LLVM.

namespace sammine_lang {

/// `sammine-mir-to-llvm`: MIR's types become LLVM's, in function signatures,
/// blocks, calls, returns and branches; MIR's ops become LLVM's: memory ops
/// malloc, free, loads and stores, sammine.call a func.call, or a call to C
/// or a kernel's wrapper as they take their arguments, a string literal the
/// address of a constant global.
std::unique_ptr<mlir::Pass> createMIRToLLVMPass();

/// The LLVM type the pass converts MIR's `type` to, or a null type if it
/// can't: for what has to be in LLVM's types before the pass runs, e.g. a C
/// function's declaration.
mlir::Type llvmTypeOf(mlir::Type type);

/// Makes the pass available to tools by its name, e.g. sammine-opt.
void registerMIRToLLVMPass();

} // namespace sammine_lang
