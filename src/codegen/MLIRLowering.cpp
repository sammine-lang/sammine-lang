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

/// Check if the module contains any tensor or linalg ops that need
/// bufferization. CPU-only modules (no kernel code) skip the entire
/// bufferization pipeline to avoid analysis failures on LLVM dialect ops.
static bool moduleHasTensorOps(mlir::ModuleOp module) {
  bool found = false;
  module.walk([&](mlir::Operation *op) {
    if (llvm::isa<mlir::linalg::LinalgDialect, mlir::tensor::TensorDialect>(
            op->getDialect())) {
      found = true;
      return mlir::WalkResult::interrupt();
    }
    // Also check for tensor-typed results on any op
    for (auto result : op->getResults()) {
      if (mlir::isa<mlir::RankedTensorType>(result.getType())) {
        found = true;
        return mlir::WalkResult::interrupt();
      }
    }
    return mlir::WalkResult::advance();
  });
  return found;
}

std::unique_ptr<llvm::Module>
lowerMLIRToLLVMIR(mlir::ModuleOp module, llvm::LLVMContext &llvmCtx) {
  auto *context = module->getContext();

  // Run lowering passes
  mlir::PassManager pm(context);

  // Phase 7a: tensor/linalg → memref/loops
  // Only run when the module contains tensor/linalg ops (kernel code).
  // CPU-only modules skip this entirely.
  if (moduleHasTensorOps(module)) {
    // Step A: Module-level bufferization (tensor → memref)
    // Uses bufferizeFunctionBoundaries to correctly update function signatures.
    // Only functions marked with "sammine.kernel" are analyzed; all others are
    // excluded to prevent crashes on cf.br loops in CPU code.
    // Excluded functions get copyBeforeWrite semantics (safe, no analysis).
    {
      std::vector<std::string> noAnalysisFuncs;
      module.walk([&](mlir::func::FuncOp funcOp) {
        if (!funcOp->hasAttr("sammine.kernel"))
          noAnalysisFuncs.push_back(funcOp.getName().str());
      });

      mlir::bufferization::OneShotBufferizationOptions bufOpts;
      bufOpts.bufferizeFunctionBoundaries = true;
      bufOpts.allowUnknownOps = true;
      bufOpts.noAnalysisFuncFilter = noAnalysisFuncs;

      mlir::bufferization::BufferizationState state;
      if (mlir::failed(mlir::bufferization::runOneShotModuleBufferize(
              module, bufOpts, state))) {
        module.emitError("Module bufferization failed");
        return nullptr;
      }
    }

    // Step B: Linalg → loops (runs per-function, only affects funcs with
    // linalg ops; no-op for CPU functions)
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::createConvertLinalgToLoopsPass());

    // Step B2: Buffer optimization passes (all operate on func::FuncOp)
    // Move allocations out of loops (10k iterations → 1 alloc)
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::bufferization::createBufferHoistingPass());
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::bufferization::createBufferLoopHoistingPass());
    // Convert memref.alloc (malloc) → memref.alloca (stack) for small static
    // arrays. Default maxAllocSizeInBytes=1024; our kernel arrays are up to
    // 8KB (1000 x f64), so raise to 64KB to cover realistic array sizes.
    {
      mlir::bufferization::PromoteBuffersToStackPassOptions stackOpts;
      stackOpts.maxAllocSizeInBytes = 65536;
      pm.addNestedPass<mlir::func::FuncOp>(
          mlir::bufferization::createPromoteBuffersToStackPass(stackOpts));
    }

    // Note: Ownership-based buffer deallocation is NOT used because it
    // crashes on CPU functions with cf.br loops (same limitation as the
    // deprecated pipeline). Since promote-buffers-to-stack converts all
    // small static allocations to alloca (no free needed), this is fine
    // for current use cases. If large dynamic allocations are added in
    // the future, a selective deallocation pass will be needed.

    // Step C: Cleanup passes needed before memref → LLVM
    pm.addPass(mlir::memref::createExpandStridedMetadataPass());
    pm.addPass(mlir::createLowerAffinePass());
  }

  // Existing: scf/arith/cf/memref/func → llvm
  pm.addPass(mlir::createSCFToControlFlowPass());
  pm.addPass(mlir::createArithToLLVMConversionPass());
  pm.addPass(mlir::createConvertControlFlowToLLVMPass());
  pm.addPass(mlir::createFinalizeMemRefToLLVMConversionPass());
  pm.addPass(mlir::createConvertFuncToLLVMPass());
  pm.addPass(mlir::createReconcileUnrealizedCastsPass());

  if (mlir::failed(pm.run(module)))
    return nullptr;

  // Register translations
  mlir::registerBuiltinDialectTranslation(*context);
  mlir::registerLLVMDialectTranslation(*context);

  // Export to LLVM IR
  auto llvmModule = mlir::translateModuleToLLVMIR(module, llvmCtx);
  if (!llvmModule)
    return nullptr;

  // Annotate all functions with nounwind (sammine has no exceptions).
  // Mark malloc's return as noalias.
  for (auto &F : *llvmModule) {
    F.addFnAttr(llvm::Attribute::NoUnwind);
    if (F.getName() == "malloc")
      F.addRetAttr(llvm::Attribute::NoAlias);
  }

  return llvmModule;
}

} // namespace sammine_lang
