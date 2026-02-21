//
// Created by Jasmine Tang on 3/29/24.
//

#include "codegen/SammineJIT.h"

#include "llvm/ExecutionEngine/Orc/CompileUtils.h" // Provides the SimpleCompiler class.
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h" // Provides the DynamicLibrarySearchGenerator class.
#include "llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h" // Provides SelfExecutorProcessControl
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
//! \file SammineJIT.cpp
//! \brief The Implementation for SammineJIT
namespace sammine_lang {

SammineJIT::SammineJIT(std::unique_ptr<llvm::orc::ExecutionSession> ES,
                       llvm::orc::JITTargetMachineBuilder JTMB,
                       llvm::DataLayout DL)
    : ES(std::move(ES)),
      ObjectLayer(*this->ES,
                  [](const llvm::MemoryBuffer &) {
                    return std::make_unique<llvm::SectionMemoryManager>();
                  }),
      CompileLayer(
          *this->ES, ObjectLayer,
          std::make_unique<llvm::orc::ConcurrentIRCompiler>(std::move(JTMB))),
      DL(std::move(DL)), Mangle(*this->ES, this->DL),
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

} // namespace sammine_lang
