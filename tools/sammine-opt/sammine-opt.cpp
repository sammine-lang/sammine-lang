//! \file sammine-opt.cpp
//! \brief mlir-opt for the sammine dialect: parses MIR, runs the passes it's
//! asked for, prints the result. Tests MIR on small hand-written inputs.

#include "dialect/ElaborateDrops.h"
#include "dialect/LambdaSets.h"
#include "dialect/MIRToLLVM.h"
#include "dialect/OwnershipCheck.h"
#include "dialect/SammineDialect.h"
#include "dialect/SizeClosures.h"
#include "dialect/ViewCheck.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

int main(int argc, char **argv) {
  sammine_lang::registerMIRToLLVMPass();
  sammine_lang::registerSizeClosuresPass();
  sammine_lang::registerSpecializeLambdaSetsPass();
  sammine_lang::registerLowerLambdaSetsPass();
  sammine_lang::registerCheckOwnershipPass();
  sammine_lang::registerCheckViewsPass();
  sammine_lang::registerElaborateDropsPass();
  sammine_lang::registerExpandDropsPass();
  mlir::DialectRegistry registry;
  // MIR meets the LLVM dialect where it calls, or is called from, code that
  // didn't go through it.
  registry.insert<sammine_lang::smn::SammineDialect, mlir::func::FuncDialect,
                  mlir::arith::ArithDialect, mlir::cf::ControlFlowDialect,
                  mlir::LLVM::LLVMDialect>();
  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "sammine MIR optimizer\n", registry));
}
