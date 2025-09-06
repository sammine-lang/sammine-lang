#pragma once

#include "codegen/SammineJIT.h"
#include "iostream"
#include "util/Utilities.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include <map>
//! \file LLVMRes.h
//! \brief Defined LLVMRes, which encapsulates the state of LLVM (Context,
//! Modules, IRBuilder, PassManagers, JIT) context base information. This
//! resource will be shared across multiple instances within the Compiler
namespace sammine_lang {
class LLVMRes {
public:
  llvm::ExitOnError ExitOnErr;
  std::unique_ptr<llvm::LLVMContext> Context;
  std::unique_ptr<llvm::IRBuilder<>> Builder;
  std::unique_ptr<llvm::Module> Module;

  std::unique_ptr<llvm::FunctionPassManager> FnPass;
  std::unique_ptr<llvm::LoopAnalysisManager> LpAnalysis;
  std::unique_ptr<llvm::FunctionAnalysisManager> FnAnalysis;
  std::unique_ptr<llvm::CGSCCAnalysisManager> CgAnalysis;
  std::unique_ptr<llvm::ModuleAnalysisManager> ModuleAnalysis;
  std::unique_ptr<llvm::PassInstrumentationCallbacks> PassCallbacks;
  std::unique_ptr<llvm::StandardInstrumentations> StdIns;
  std::unique_ptr<SammineJIT> sammineJIT;

  llvm::PassBuilder PB;
  std::unique_ptr<llvm::TargetMachine> target_machine;
  llvm::legacy::PassManager pass;
  std::error_code EC;
  LLVMRes() {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeNativeTargetAsmParser();
    llvm::InitializeNativeTargetAsmPrinter();

    sammineJIT = ExitOnErr(SammineJIT::Create());
    InitializeModuleAndManagers();
  }
  void InitializeModuleAndManagers() {
    InitializeEssentials();

    // InitializeManagers();
    //
    // InitializeInstrs();

    // Add transform passes.
    // Do simple "peephole" optimizations and bit-twiddling optzns.
    //  FnPass->addPass(llvm::InstCombinePass());
    //  // Reassociate expressions.
    //  FnPass->addPass(llvm::ReassociatePass());
    //  // Eliminate Common SubExpressions.
    //  FnPass->addPass(llvm::GVNPass());
    //  // Simplify the control flow graph (deleting unreachable blocks, etc).
    //  FnPass->addPass(llvm::SimplifyCFGPass());

    // InitializePassBuilder();
  }
  void InitializeEssentials() {
    // Open a new context and module.

    Context = std::make_unique<llvm::LLVMContext>();
    assert(Context);

    Module = std::make_unique<llvm::Module>("KaleidoscopeJIT", *Context);
    assert(Module);
    auto TargetTripleStr = LLVMGetDefaultTargetTriple();
    llvm::Triple TargetTriple(TargetTripleStr);
    std::string Error;
    auto Target = llvm::TargetRegistry::lookupTarget(TargetTripleStr, Error);

    auto CPU = "generic";
    auto Features = "";
    llvm::TargetOptions opt;
    target_machine =
        std::unique_ptr<llvm::TargetMachine>(Target->createTargetMachine(
            TargetTriple, CPU, Features, opt, llvm::Reloc::PIC_));

    // Create a new builder for the module.
    Builder = std::make_unique<llvm::IRBuilder<>>(*Context);
    assert(Builder);

    Module->setDataLayout(target_machine->createDataLayout());
  }
  void InitializeManagers() {
    // Create new pass and analysis managers.
    FnPass = std::make_unique<llvm::FunctionPassManager>();
    LpAnalysis = std::make_unique<llvm::LoopAnalysisManager>();
    FnAnalysis = std::make_unique<llvm::FunctionAnalysisManager>();
    CgAnalysis = std::make_unique<llvm::CGSCCAnalysisManager>();
    ModuleAnalysis = std::make_unique<llvm::ModuleAnalysisManager>();
  }

  void InitializeInstrs() {
    PassCallbacks = std::make_unique<llvm::PassInstrumentationCallbacks>();

    StdIns =
        std::make_unique<llvm::StandardInstrumentations>(*Context,
                                                         /*DebugLogging*/ true);
    StdIns->registerCallbacks(*PassCallbacks, ModuleAnalysis.get());
  }
  void InitializePassBuilder() {
    // Register analysis passes used in these transform passes.
    PB.registerModuleAnalyses(*ModuleAnalysis);
    PB.registerFunctionAnalyses(*FnAnalysis);
    PB.crossRegisterProxies(*LpAnalysis, *FnAnalysis, *CgAnalysis,
                            *ModuleAnalysis);
  }
};
} // namespace sammine_lang
