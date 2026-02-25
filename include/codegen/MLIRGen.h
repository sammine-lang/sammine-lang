#pragma once

#include "ast/Ast.h"
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

/// Emit MLIR for the given type-checked AST. Returns a newly created MLIR
/// module, or nullptr on failure.
mlir::OwningOpRef<mlir::ModuleOp>
mlirGen(mlir::MLIRContext &context, AST::ProgramAST *program,
        const std::string &moduleName, const std::string &fileName,
        const std::string &sourceText);

} // namespace sammine_lang
