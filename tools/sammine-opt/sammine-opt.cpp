//! \file sammine-opt.cpp
//! \brief mlir-opt for the sammine dialect: parses MIR, runs the passes it's
//! asked for, prints the result. Tests MIR on small hand-written inputs.

#include "codegen/SammineDialect.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"
#include "llvm/Support/Compiler.h"

#if LLVM_ADDRESS_SANITIZER_BUILD
// With SAMMINE_SANITIZE, this tool is built with ASan but LLVM isn't. LLVM's
// allocators poison and unpoison their memory only in code built with ASan,
// so copies of their inline code compiled here and in LLVM disagree, and ASan
// reports a use-after-poison on valid memory as soon as an MLIRContext is
// made. Ignoring such manual poisoning keeps every other check.
extern "C" const char *__asan_default_options() {
  return "allow_user_poisoning=0";
}
#endif

int main(int argc, char **argv) {
  mlir::DialectRegistry registry;
  registry.insert<sammine_lang::smn::SammineDialect, mlir::func::FuncDialect,
                  mlir::arith::ArithDialect, mlir::cf::ControlFlowDialect>();
  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "sammine MIR optimizer\n", registry));
}
