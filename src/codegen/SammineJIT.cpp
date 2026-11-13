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

llvm::Error SammineJIT::loadLibrary(const std::string &path) {
  if (llvm::StringRef(path).ends_with(".a")) {
    auto archive = llvm::orc::StaticLibraryDefinitionGenerator::Load(
        ObjectLayer, path.c_str());
    if (!archive)
      return archive.takeError();
    MainJD.addGenerator(std::move(*archive));
    return llvm::Error::success();
  }
  auto shared = llvm::orc::DynamicLibrarySearchGenerator::Load(
      path.c_str(), DL.getGlobalPrefix());
  if (!shared)
    return shared.takeError();
  MainJD.addGenerator(std::move(*shared));
  return llvm::Error::success();
}

int SammineJIT::execute_main(llvm::orc::ThreadSafeModule module,
                             const std::vector<std::string> &libraries,
                             const std::vector<std::string> &jit_args) {
  using sammine_util::Location;

  // Load imported module libraries into the JIT.
  for (auto &lib : libraries) {
    LOG({ fmt::println("Loading {}", lib); });
    if (auto err = loadLibrary(lib)) {
      add_error(Location::NonPrintable(),
                fmt::format("JIT failed to load library '{}': {}", lib,
                            toString(std::move(err))));
      return 1;
    }
  }

  // Add the compiled module to the JIT.
  if (auto err = addModule(std::move(module))) {
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

  // Let the executor call main, with argv as the program name followed by the
  // --jit-args.
  std::vector<std::string> args{"sammine"};
  args.insert(args.end(), jit_args.begin(), jit_args.end());
  auto result = ES->getExecutorProcessControl().runAsMain(
      mainSym->getAddress(), args);
  if (!result) {
    add_error(Location::NonPrintable(),
              fmt::format("JIT failed to run main: {}",
                          toString(result.takeError())));
    return 1;
  }
  exit_code_ = *result;
  return exit_code_;
}

} // namespace sammine_lang
