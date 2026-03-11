//
// Created by Jasmine Tang on 3/29/24.
//

#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/Core.h" // Core utilities such as ExecutionSession and JITDylib.
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
namespace sammine_lang {

//! \file SammineJIT.h
//! \brief JIT Compiler based on Kaleidoscope
class SammineJIT {
private:
  std::unique_ptr<llvm::orc::ExecutionSession> ES;
  llvm::orc::RTDyldObjectLinkingLayer ObjectLayer;
  llvm::orc::IRCompileLayer CompileLayer;

  llvm::DataLayout DL;
  llvm::orc::MangleAndInterner Mangle;
  llvm::orc::JITDylib &MainJD;

public:
  SammineJIT(std::unique_ptr<llvm::orc::ExecutionSession> ES,
             llvm::orc::JITTargetMachineBuilder JTMB, llvm::DataLayout DL);

  ~SammineJIT();

  static llvm::Expected<std::unique_ptr<SammineJIT>> Create();

  const llvm::DataLayout &getDataLayout() const { return DL; }

  llvm::orc::JITDylib &getMainJITDylib() { return MainJD; }

  llvm::Error addModule(llvm::orc::ThreadSafeModule TSM,
                        llvm::orc::ResourceTrackerSP RT = nullptr);

  llvm::Expected<llvm::orc::ExecutorSymbolDef> lookup(llvm::StringRef Name) {
    return ES->lookup({&MainJD}, Mangle(Name.str()));
  }
};

} // namespace sammine_lang
