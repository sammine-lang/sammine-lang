#pragma once

#include "codegen/SammineJIT.h"
#include "util/Utilities.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
//! \file LLVMRes.h
//! \brief Defined LLVMRes, which encapsulates the state of LLVM (Context,
//! Modules, PassManagers, JIT) context base information. This
//! resource will be shared across multiple instances within the Compiler
namespace sammine_lang {
class LLVMRes {
public:
  llvm::ExitOnError ExitOnErr;
  std::unique_ptr<llvm::LLVMContext> Context;
  std::unique_ptr<llvm::Module> Module;

  std::unique_ptr<SammineJIT> sammineJIT;

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
    InitializeEssentials();
  }

private:
  void InitializeEssentials() {
    Context = std::make_unique<llvm::LLVMContext>();
    assert(Context);

    Module = std::make_unique<llvm::Module>("KaleidoscopeJIT", *Context);
    assert(Module);
    llvm::Triple TargetTriple(LLVMGetDefaultTargetTriple());
    std::string Error;
    auto Target = llvm::TargetRegistry::lookupTarget(TargetTriple, Error);

    auto CPU = "generic";
    auto Features = "";
    llvm::TargetOptions opt;
    target_machine =
        std::unique_ptr<llvm::TargetMachine>(Target->createTargetMachine(
            TargetTriple, CPU, Features, opt, llvm::Reloc::PIC_));

    Module->setDataLayout(target_machine->createDataLayout());
  }
};
} // namespace sammine_lang
