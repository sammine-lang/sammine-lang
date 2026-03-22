//
// Created by Jasmine Tang on 3/29/24.
//

#pragma once

#include "util/Utilities.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/Core.h" // Core utilities such as ExecutionSession and JITDylib.
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include <vector>
namespace sammine_lang {

//! \file SammineJIT.h
//! \brief JIT Compiler based on Kaleidoscope
class SammineJIT : public sammine_util::Reportee {
private:
  std::unique_ptr<llvm::orc::ExecutionSession> ES;
  llvm::orc::RTDyldObjectLinkingLayer ObjectLayer;
  llvm::orc::IRCompileLayer CompileLayer;

  llvm::DataLayout DL;
  llvm::orc::MangleAndInterner Mangle;
  llvm::orc::JITDylib &MainJD;

  int exit_code_ = 0;

public:
  SammineJIT(std::unique_ptr<llvm::orc::ExecutionSession> ES,
             llvm::orc::JITTargetMachineBuilder JTMB, llvm::DataLayout DL);

  ~SammineJIT() override;

  static llvm::Expected<std::unique_ptr<SammineJIT>> Create();

  const llvm::DataLayout &getDataLayout() const { return DL; }

  llvm::orc::JITDylib &getMainJITDylib() { return MainJD; }

  llvm::Error addModule(llvm::orc::ThreadSafeModule TSM,
                        llvm::orc::ResourceTrackerSP RT = nullptr);

  llvm::Error loadLibrary(llvm::StringRef Path) {
    auto G = llvm::orc::DynamicLibrarySearchGenerator::Load(
        Path.data(), DL.getGlobalPrefix());
    if (!G)
      return G.takeError();
    MainJD.addGenerator(std::move(*G));
    return llvm::Error::success();
  }

  llvm::Expected<llvm::orc::ExecutorSymbolDef> lookup(llvm::StringRef Name) {
    return ES->lookup({&MainJD}, Mangle(Name.str()));
  }

  int execute_main(std::unique_ptr<llvm::Module> module,
                   std::unique_ptr<llvm::LLVMContext> context,
                   const std::vector<std::string> &libraries,
                   const std::vector<std::string> &program_args = {});

  [[nodiscard]] int get_exit_code() const { return exit_code_; }
};

} // namespace sammine_lang
