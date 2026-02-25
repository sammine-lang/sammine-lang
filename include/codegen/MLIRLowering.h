#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <memory>

namespace sammine_lang {

/// Lower an MLIR module (arith/func dialects) to LLVM IR.
std::unique_ptr<llvm::Module>
lowerMLIRToLLVMIR(mlir::ModuleOp module, llvm::LLVMContext &llvmCtx);

} // namespace sammine_lang
