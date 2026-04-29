#include "codegen/LinalgReduceToGpu.h"
#include "codegen/LowerSammineOps.h"
#include "codegen/MLIRLowering.h"
#include "compiler/Compiler.h"
#include "util/Utilities.h"

#include "stablehlo/conversions/linalg/transforms/Passes.h"

#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Bufferization/IR/BufferizableOpInterface.h"
#include "mlir/Dialect/Bufferization/Pipelines/Passes.h"
#include "mlir/Dialect/Bufferization/Transforms/OneShotAnalysis.h"
#include "mlir/Dialect/Bufferization/Transforms/OneShotModuleBufferize.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Conversion/GPUCommon/GPUCommonPass.h"
#include "mlir/Conversion/ConvertToLLVM/ToLLVMPass.h"
#include "mlir/Conversion/GPUCommon/GPUToLLVM.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/Conversion/Passes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/GPU/GPUToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/NVVM/NVVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/Passes.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

namespace sammine_lang {

static bool isGPU(GPUMode gpuTarget) {
  return gpuTarget != GPUMode::NONE;
}


std::unique_ptr<llvm::Module> lowerMLIRToLLVMIR(mlir::ModuleOp cpuModule,
                                                mlir::ModuleOp kernelModule,
                                                llvm::LLVMContext &llvmCtx,
                                                GPUMode gpuTarget) {
  auto *context = cpuModule->getContext();

  // Phase 1: Process kernel module (if present)
  if (kernelModule) {
    // Step A0: Lower StableHLO ops to linalg before bufferization.
    {
      mlir::PassManager stablehloPM(context);
      mlir::stablehlo::StablehloLegalizeToLinalgPassOptions opts;
      opts.enablePrimitiveOps = true;
      stablehloPM.addPass(
          mlir::stablehlo::createStablehloLegalizeToLinalgPass(opts));
      if (mlir::failed(stablehloPM.run(kernelModule)))
        return nullptr;
    }

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

    // Step B: Lower linalg ops (GPU or CPU path)
    if (isGPU(gpuTarget)) {
      // gpu-module-to-binary internally translates to LLVM IR, so register
      // all dialect translations before running the pass pipeline.
      mlir::registerBuiltinDialectTranslation(*context);
      mlir::registerLLVMDialectTranslation(*context);
      mlir::registerGPUDialectTranslation(*context);
      mlir::registerNVVMDialectTranslation(*context);

      // Lower linalg.reduce → gpu.launch + gpu.all_reduce before
      // linalg-to-parallel-loops (which only handles parallel iterators).
      if (mlir::failed(lowerLinalgReduceToGpuLaunch(kernelModule)))
        return nullptr;

      // GPU path: linalg → parallel loops → gpu.launch → NVVM → binary
      mlir::PassManager kernelPM(context);

      kernelPM.addNestedPass<mlir::func::FuncOp>(
          mlir::createConvertLinalgToParallelLoopsPass());
      kernelPM.addNestedPass<mlir::func::FuncOp>(
          mlir::createGpuMapParallelLoopsPass());
      kernelPM.addPass(mlir::createConvertParallelLoopToGpuPass());
      kernelPM.addPass(mlir::createGpuKernelOutliningPass());
      kernelPM.addPass(mlir::createGpuNVVMAttachTarget());
      kernelPM.addNestedPass<mlir::gpu::GPUModuleOp>(
          mlir::createConvertGpuOpsToNVVMOps());
      // Lower remaining non-LLVM ops inside gpu.module before serialization
      kernelPM.addNestedPass<mlir::gpu::GPUModuleOp>(
          mlir::createLowerAffinePass());
      kernelPM.addNestedPass<mlir::gpu::GPUModuleOp>(
          mlir::createArithToLLVMConversionPass());
      kernelPM.addNestedPass<mlir::gpu::GPUModuleOp>(
          mlir::createConvertIndexToLLVMPass());
      kernelPM.addNestedPass<mlir::gpu::GPUModuleOp>(
          mlir::createReconcileUnrealizedCastsPass());
      kernelPM.addPass(mlir::createGpuModuleToBinaryPass());

      kernelPM.addPass(mlir::memref::createExpandStridedMetadataPass());
      kernelPM.addPass(mlir::createLowerAffinePass());

      if (mlir::failed(kernelPM.run(kernelModule)))
        return nullptr;
    } else {
      // CPU path: linalg → sequential loops + buffer optimization
      mlir::PassManager kernelPM(context);

      kernelPM.addNestedPass<mlir::func::FuncOp>(
          mlir::createConvertLinalgToLoopsPass());

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

      kernelPM.addPass(mlir::memref::createExpandStridedMetadataPass());
      kernelPM.addPass(mlir::createLowerAffinePass());

      if (mlir::failed(kernelPM.run(kernelModule)))
        return nullptr;
    }

    // Step C: Merge kernel functions (and gpu.module ops) into CPU module.
    // After GPU lowering, functions may be llvm.func (not func.func).
    for (auto &op :
         llvm::make_early_inc_range(kernelModule.getBody()->getOperations())) {
      if (auto funcOp = llvm::dyn_cast<mlir::func::FuncOp>(op)) {
        auto name = funcOp.getName();
        if (auto existing = cpuModule.lookupSymbol<mlir::func::FuncOp>(name))
          existing.erase();
        funcOp->remove();
        cpuModule.push_back(funcOp);
      } else if (llvm::isa<mlir::gpu::GPUModuleOp>(op) ||
                 llvm::isa<mlir::gpu::BinaryOp>(op)) {
        op.remove();
        cpuModule.push_back(&op);
      }
    }
    // gpu.launch_func requires gpu.container_module on the parent module.
    if (isGPU(gpuTarget))
      cpuModule->setAttr("gpu.container_module",
                         mlir::UnitAttr::get(context));
  }

  // Phase 1.5: Lower sammine.to_device / sammine.to_host → gpu ops.
  // Runs after merge so the wrapper functions (which contain the sammine ops)
  // are in the CPU module alongside the kernel functions they reference.
  {
    mlir::PassManager pm(context);
    pm.addPass(createLowerSammineOpsPass());
    if (mlir::failed(pm.run(cpuModule)))
      return nullptr;
  }

  // Phase 2: Lower everything to LLVM (on the merged CPU module)
  if (isGPU(gpuTarget)) {
    mlir::PassManager pm(context);
    pm.addPass(mlir::createSCFToControlFlowPass());
    pm.addPass(mlir::createGpuToLLVMConversionPass());
    pm.addPass(mlir::createReconcileUnrealizedCastsPass());
    if (mlir::failed(pm.run(cpuModule)))
      return nullptr;
  } else {
    mlir::PassManager pm(context);
    pm.addPass(mlir::createSCFToControlFlowPass());
    pm.addPass(mlir::createArithToLLVMConversionPass());
    pm.addPass(mlir::createConvertControlFlowToLLVMPass());
    pm.addPass(mlir::createFinalizeMemRefToLLVMConversionPass());
    pm.addPass(mlir::createConvertFuncToLLVMPass());
    pm.addPass(mlir::createReconcileUnrealizedCastsPass());
    if (mlir::failed(pm.run(cpuModule)))
      return nullptr;
  }

  // Register translations
  mlir::registerBuiltinDialectTranslation(*context);
  mlir::registerLLVMDialectTranslation(*context);
  if (isGPU(gpuTarget)) {
    mlir::registerGPUDialectTranslation(*context);
    mlir::registerNVVMDialectTranslation(*context);
  }

  // Export to LLVM IR
  auto llvmModule = mlir::translateModuleToLLVMIR(cpuModule, llvmCtx);
  if (!llvmModule)
    return nullptr;

  // GPU: replace global_dtors for GPU module unload with an explicit call
  // at the end of main(). MLIR registers cuModuleUnload as a global destructor
  // (priority 123), but it races with CUDA driver teardown — the driver
  // deinitializes before the destructor runs, causing cuModuleUnload to fail.
  // Moving the unload into main() ensures proper ordering.
  if (isGPU(gpuTarget)) {
    if (auto *dtors = llvmModule->getNamedGlobal("llvm.global_dtors")) {
      // Collect unload functions from global_dtors
      llvm::SmallVector<llvm::Function *, 2> unloadFns;
      if (auto *init = dtors->getInitializer()) {
        if (auto *arr = llvm::dyn_cast<llvm::ConstantArray>(init)) {
          for (unsigned i = 0; i < arr->getNumOperands(); i++) {
            auto *entry = llvm::cast<llvm::ConstantStruct>(arr->getOperand(i));
            if (auto *fn = llvm::dyn_cast<llvm::Function>(entry->getOperand(1)))
              unloadFns.push_back(fn);
          }
        }
      }
      dtors->eraseFromParent();

      // Insert calls to unload functions before each return in main()
      if (auto *mainFn = llvmModule->getFunction("main")) {
        for (auto &BB : *mainFn) {
          if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
            llvm::IRBuilder<> irBuilder(ret);
            for (auto *fn : unloadFns)
              irBuilder.CreateCall(fn);
          }
        }
      }
    }
  }

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
