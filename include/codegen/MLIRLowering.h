#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <memory>

namespace sammine_lang {

/// Lower MLIR modules to LLVM IR.
/// If kernelModule is non-null, it is bufferized and lowered separately,
/// then merged into cpuModule before final LLVM translation.
std::unique_ptr<llvm::Module>
lowerMLIRToLLVMIR(mlir::ModuleOp cpuModule, mlir::ModuleOp kernelModule,
                  llvm::LLVMContext &llvmCtx);

} // namespace sammine_lang
