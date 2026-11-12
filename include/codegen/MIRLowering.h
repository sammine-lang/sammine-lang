#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "codegen/SourceLocator.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringMap.h"

#include <optional>

//! \file MIRLowering.h
//! \brief Lowers a type-checked function's body to MIR (see
//! knowledge_base/plans/mir.md). MIR grows one construct at a time: a body
//! using anything not handled yet is left to the AST codegen.

namespace sammine_lang {

class MIRLowering {
public:
  /// Marks a function whose body was lowered through MIR.
  static constexpr llvm::StringLiteral kMirAttr = "sammine.mir";

  MIRLowering(mlir::OpBuilder &builder, const AST::ASTProperties &props,
              const SourceLocator &locator)
      : builder_(builder), props_(props), locator_(locator) {}

  /// Builds `ast`'s body into `func`, which has its signature and no body. On
  /// success `func` is marked `kMirAttr`. Returns false, leaving `func`
  /// without a body, if `ast` uses something MIR can't lower yet.
  bool lowerBody(AST::FuncDefAST *ast, mlir::func::FuncOp func);

private:
  mlir::OpBuilder &builder_;
  const AST::ASTProperties &props_;
  const SourceLocator &locator_;
  /// What each name in the current function stands for: its parameters and
  /// `let`s so far. A later `let` of a name shadows the earlier one.
  llvm::StringMap<mlir::Value> names_;

  mlir::Location loc(const AST::AstBase *ast) const {
    return locator_.loc(builder_, ast);
  }
  /// The MIR type of a sammine type MIR handles so far, or nullopt.
  std::optional<mlir::Type> mirType(const Type &type) const;
  bool supportsSignature(AST::FuncDefAST *ast) const;

  mlir::LogicalResult lowerBlock(AST::BlockAST *block);
  mlir::LogicalResult lowerVarDef(AST::VarDefAST *def);
  mlir::LogicalResult lowerReturn(AST::ReturnStmtAST *ret);
  mlir::FailureOr<mlir::Value> lowerExpr(AST::ExprAST *expr);
  mlir::FailureOr<mlir::Value> lowerNumber(AST::NumberExprAST *num);
  mlir::FailureOr<mlir::Value> lowerBool(AST::BoolExprAST *b);
  mlir::FailureOr<mlir::Value> lowerChar(AST::CharExprAST *c);
  mlir::FailureOr<mlir::Value> lowerVariable(AST::VariableExprAST *var);
  mlir::FailureOr<mlir::Value> lowerBinary(AST::BinaryExprAST *bin);
};

} // namespace sammine_lang
