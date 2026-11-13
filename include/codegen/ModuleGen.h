#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "codegen/SourceLocator.h"
#include "compiler/Options.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"

#include <string>

//! \file ModuleGen.h
//! \brief Builds a type-checked program's MLIR: each function's body through
//! MIR, kernels through KernelGen, and the module around them.

namespace sammine_lang {

/// A program's MLIR: its CPU module, and its kernels' module, null if it has
/// no kernel.
struct GeneratedModules {
  mlir::OwningOpRef<mlir::ModuleOp> cpuModule;
  mlir::OwningOpRef<mlir::ModuleOp> kernelModule;
};

/// The MLIR for `program`, or an empty result if a module doesn't verify.
/// `moduleName` is the library's name, empty for an executable. Ops are
/// located by `locator`.
GeneratedModules
generateModules(mlir::MLIRContext &context, AST::ProgramAST *program,
                const std::string &moduleName, const SourceLocator &locator,
                const std::string &fileName, const std::string &sourceText,
                const AST::ASTProperties &props, GPUMode gpuMode);

} // namespace sammine_lang
