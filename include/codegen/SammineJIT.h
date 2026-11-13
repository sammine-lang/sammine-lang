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

  /// Makes what the library at `path` defines found by the program: a
  /// shared library is loaded into the process, a static archive's objects
  /// are linked in as the program names what they define.
  llvm::Error loadLibrary(const std::string &path);

  llvm::Expected<llvm::orc::ExecutorSymbolDef> lookup(llvm::StringRef Name) {
    return ES->lookup({&MainJD}, Mangle(Name.str()));
  }

  /// Runs `module`'s main. The module comes with its context, as one
  /// value: it must end before the context does, on every way out of here,
  /// and the two apart could end in either order.
  int execute_main(llvm::orc::ThreadSafeModule module,
                   const std::vector<std::string> &libraries,
                   const std::vector<std::string> &program_args = {});

  [[nodiscard]] int get_exit_code() const { return exit_code_; }
};

} // namespace sammine_lang
