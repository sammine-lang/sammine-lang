#pragma once

#include "compiler/Compiler.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <memory>

namespace sammine_lang {

/// Lower MLIR modules to LLVM IR.
/// If kernelModule is non-null, it is bufferized and lowered separately,
/// then merged into cpuModule before final LLVM translation.
/// gpuTarget: "" for CPU, "cuda" for NVIDIA, "amd" for AMD GPU.
std::unique_ptr<llvm::Module> lowerMLIRToLLVMIR(mlir::ModuleOp cpuModule,
                                                mlir::ModuleOp kernelModule,
                                                llvm::LLVMContext &llvmCtx,
                                                GPUMode gpu_mode);

} // namespace sammine_lang
