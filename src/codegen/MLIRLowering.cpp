#include "codegen/MLIRLowering.h"

#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Bufferization/IR/BufferizableOpInterface.h"
#include "mlir/Dialect/Bufferization/Pipelines/Passes.h"
#include "mlir/Dialect/Bufferization/Transforms/OneShotAnalysis.h"
#include "mlir/Dialect/Bufferization/Transforms/OneShotModuleBufferize.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/Passes.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

namespace sammine_lang {

std::unique_ptr<llvm::Module>
lowerMLIRToLLVMIR(mlir::ModuleOp cpuModule, mlir::ModuleOp kernelModule,
                  llvm::LLVMContext &llvmCtx) {
  auto *context = cpuModule->getContext();

  // Phase 1: Process kernel module (if present)
  if (kernelModule) {
    // Step A: Bufferize the kernel module (clean — no ad-hoc filters needed).
    // The kernel module contains only func/tensor/linalg/arith ops,
    // so full analysis works without allowUnknownOps or noAnalysisFuncFilter.
    {
      mlir::bufferization::OneShotBufferizationOptions bufOpts;
      bufOpts.bufferizeFunctionBoundaries = true;
      // Use identity layout (memref<NxT>) at function boundaries instead
      // of the default inferred layout (memref<NxT, strided<[?], offset: ?>>).
      // Our memrefs always have identity layout (offset=0, stride=1) since
      // buildMemrefFromPtr constructs them that way.
      bufOpts.setFunctionBoundaryTypeConversion(
          mlir::bufferization::LayoutMapOption::IdentityLayoutMap);

      mlir::bufferization::BufferizationState state;
      if (mlir::failed(mlir::bufferization::runOneShotModuleBufferize(
              kernelModule, bufOpts, state))) {
        kernelModule.emitError("Kernel module bufferization failed");
        return nullptr;
      }
    }

    // Step B: Linalg → loops + buffer optimization (on kernel module only)
    {
      mlir::PassManager kernelPM(context);

      kernelPM.addNestedPass<mlir::func::FuncOp>(
          mlir::createConvertLinalgToLoopsPass());

      // Buffer optimization: move allocations out of loops, promote to stack
      kernelPM.addNestedPass<mlir::func::FuncOp>(
          mlir::bufferization::createBufferHoistingPass());
      kernelPM.addNestedPass<mlir::func::FuncOp>(
          mlir::bufferization::createBufferLoopHoistingPass());
      {
        mlir::bufferization::PromoteBuffersToStackPassOptions stackOpts;
        stackOpts.maxAllocSizeInBytes = 65536;
        kernelPM.addNestedPass<mlir::func::FuncOp>(
            mlir::bufferization::createPromoteBuffersToStackPass(stackOpts));
      }

      // Cleanup passes needed before merging into CPU module
      kernelPM.addPass(mlir::memref::createExpandStridedMetadataPass());
      kernelPM.addPass(mlir::createLowerAffinePass());

      if (mlir::failed(kernelPM.run(kernelModule)))
        return nullptr;
    }

    // Step C: Merge kernel functions into CPU module.
    // After bufferization + lowering, kernel functions have memref signatures
    // matching the forward-declarations in the CPU module.
    for (auto &op :
         llvm::make_early_inc_range(kernelModule.getBody()->getOperations())) {
      if (auto funcOp = llvm::dyn_cast<mlir::func::FuncOp>(op)) {
        auto name = funcOp.getName();
        // Erase the memref-typed forward-declaration in CPU module
        if (auto existing =
                cpuModule.lookupSymbol<mlir::func::FuncOp>(name))
          existing.erase();
        // Move the actual definition into CPU module
        funcOp->remove();
        cpuModule.push_back(funcOp);
      }
    }
  }

  // Phase 2: Lower everything to LLVM (on the merged CPU module)
  mlir::PassManager pm(context);
  pm.addPass(mlir::createSCFToControlFlowPass());
  pm.addPass(mlir::createArithToLLVMConversionPass());
  pm.addPass(mlir::createConvertControlFlowToLLVMPass());
  pm.addPass(mlir::createFinalizeMemRefToLLVMConversionPass());
  pm.addPass(mlir::createConvertFuncToLLVMPass());
  pm.addPass(mlir::createReconcileUnrealizedCastsPass());

  if (mlir::failed(pm.run(cpuModule)))
    return nullptr;

  // Register translations
  mlir::registerBuiltinDialectTranslation(*context);
  mlir::registerLLVMDialectTranslation(*context);

  // Export to LLVM IR
  auto llvmModule = mlir::translateModuleToLLVMIR(cpuModule, llvmCtx);
  if (!llvmModule)
    return nullptr;

  // Annotate all functions with nounwind (sammine has no exceptions).
  // Mark malloc's return as noalias.
  // Kernel wrappers are noinline to preserve LICM: LLVM can hoist the
  // call out of loops when the ptr args point to loop-invariant data.
  for (auto &F : *llvmModule) {
    F.addFnAttr(llvm::Attribute::NoUnwind);
    if (F.getName() == "malloc")
      F.addRetAttr(llvm::Attribute::NoAlias);
    if (llvmModule->getFunction(("__kernel_" + F.getName()).str()))
      F.addFnAttr(llvm::Attribute::NoInline);
  }

  return llvmModule;
}

} // namespace sammine_lang
