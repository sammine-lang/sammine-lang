#pragma once

#include "mlir/Pass/Pass.h"

#include <memory>

//! \file MIRToLLVM.h
//! \brief Converts MIR's types and ops to the LLVM dialect, before the rest of
//! the module is lowered to LLVM.

namespace sammine_lang {

/// `sammine-mir-to-llvm`: !sammine.ptr becomes !llvm.ptr in function
/// signatures, calls, returns and branches. The casts the AST codegen put at
/// calls into MIR then cast !llvm.ptr to itself, and reconciling removes them.
std::unique_ptr<mlir::Pass> createMIRToLLVMPass();

/// Makes the pass available to tools by its name, e.g. sammine-opt.
void registerMIRToLLVMPass();

} // namespace sammine_lang
