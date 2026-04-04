//
// Created by Jasmine Tang on 3/29/24.
//

#include "codegen/SammineJIT.h"

#include "fmt/core.h"
#include "util/Logging.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h" // Provides the SimpleCompiler class.
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h" // Provides the DynamicLibrarySearchGenerator class.
#include "llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h" // Provides SelfExecutorProcessControl
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include <fmt/base.h>

#define DEBUG_TYPE "jit"
//! \file SammineJIT.cpp
//! \brief The Implementation for SammineJIT
namespace sammine_lang {

SammineJIT::SammineJIT(std::unique_ptr<llvm::orc::ExecutionSession> ES_,
                       llvm::orc::JITTargetMachineBuilder JTMB,
                       llvm::DataLayout DL_)
    : ES(std::move(ES_)),
      ObjectLayer(*this->ES,
                  [](const llvm::MemoryBuffer &) {
                    return std::make_unique<llvm::SectionMemoryManager>();
                  }),
      CompileLayer(
          *this->ES, ObjectLayer,
          std::make_unique<llvm::orc::ConcurrentIRCompiler>(std::move(JTMB))),
      DL(std::move(DL_)), Mangle(*this->ES, this->DL),
      MainJD(this->ES->createBareJITDylib("<main>")) {
  MainJD.addGenerator(
      cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          DL.getGlobalPrefix())));
}

SammineJIT::~SammineJIT() {
  if (auto Err = ES->endSession())
    ES->reportError(std::move(Err));
}

llvm::Expected<std::unique_ptr<SammineJIT>> SammineJIT::Create() {
  auto EPC = llvm::orc::SelfExecutorProcessControl::Create();
  if (!EPC)
    return EPC.takeError();

  auto ES = std::make_unique<llvm::orc::ExecutionSession>(std::move(*EPC));

  llvm::orc::JITTargetMachineBuilder JTMB(
      ES->getExecutorProcessControl().getTargetTriple());

  auto DL = JTMB.getDefaultDataLayoutForTarget();
  if (!DL)
    return DL.takeError();

  return std::make_unique<SammineJIT>(std::move(ES), std::move(JTMB),
                                      std::move(*DL));
}

llvm::Error SammineJIT::addModule(llvm::orc::ThreadSafeModule TSM,
                                  llvm::orc::ResourceTrackerSP RT) {
  if (!RT)
    RT = MainJD.getDefaultResourceTracker();
  return CompileLayer.add(RT, std::move(TSM));
}

int SammineJIT::execute_main(std::unique_ptr<llvm::Module> module,
                             std::unique_ptr<llvm::LLVMContext> context,
                             const std::vector<std::string> &libraries,
                             const std::vector<std::string> &jit_args) {
  using sammine_util::Location;

  // Load imported module libraries into the JIT.
  for (auto &lib : libraries) {
    if (lib.ends_with(".a")) {
      add_error(Location::NonPrintable(),
                fmt::format("JIT does not support static archives: '{}'. "
                            "Recompile the dependency as a shared library "
                            "(--lib=shared) or run without --jit.",
                            lib));
      return 1;
    }
    LOG({ fmt::println("Loading {}", lib); });
    if (auto err = loadLibrary(lib)) {
      add_error(Location::NonPrintable(),
                fmt::format("JIT failed to load library '{}': {}", lib,
                            toString(std::move(err))));
      return 1;
    }
  }

  // Add the compiled module to the JIT.
  auto TSCtx = llvm::orc::ThreadSafeContext(std::move(context));
  auto TSM = llvm::orc::ThreadSafeModule(std::move(module), TSCtx);

  if (auto err = addModule(std::move(TSM))) {
    add_error(Location::NonPrintable(), fmt::format("JIT addModule failed: {}",
                                                    toString(std::move(err))));
    return 1;
  }

  // Look up and call main().
  auto mainSym = lookup("main");
  if (!mainSym) {
    add_error(Location::NonPrintable(),
              fmt::format("JIT lookup of 'main' failed: {}",
                          toString(mainSym.takeError())));
    return 1;
  }

  auto *mainFn = mainSym->getAddress().toPtr<int (*)(int, char **)>();

  // Build argc/argv from program_args.
  std::vector<std::string> args_storage;
  args_storage.push_back("sammine");
  for (auto &a : jit_args)
    args_storage.push_back(a);

  std::vector<char *> argv;
  for (auto &a : args_storage)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  exit_code_ = mainFn(static_cast<int>(args_storage.size()), argv.data());
  return exit_code_;
}

} // namespace sammine_lang
