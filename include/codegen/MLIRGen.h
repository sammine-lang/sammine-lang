#pragma once

#include "ast/Ast.h"
#include "ast/ASTProperties.h"
#include "typecheck/Types.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Value.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"

#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace sammine_lang {

/// Result of MLIR generation: a CPU module and an optional kernel module.
struct MLIRGenResult {
  mlir::OwningOpRef<mlir::ModuleOp> cpuModule;
  /// Separate module for kernel functions (tensor/linalg).
  /// Null if the program has no kernel definitions.
  mlir::OwningOpRef<mlir::ModuleOp> kernelModule;
};

/// Emit MLIR for the given type-checked AST. Returns a CPU module and
/// (optionally) a kernel module, or an empty result on failure.
MLIRGenResult
mlirGen(mlir::MLIRContext &context, AST::ProgramAST *program,
        const std::string &moduleName, const std::string &fileName,
        const std::string &sourceText, const AST::ASTProperties &props);

} // namespace sammine_lang
