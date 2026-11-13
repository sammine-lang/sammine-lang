#pragma once
#include "ast/Ast.h"

//! \file NeverValueCheck.h
//! \brief Where an expression that never gives a value, typed Never (a
//! panic(), an `if` whose branches all panic or return), may be: where the
//! code ends, as a statement, a branch's or a block's value, what a `let`
//! binds or what a `return` returns. Not where its value is used, as an
//! argument, an operand, an element or a condition: there is none to use.
//! Runs once type checking is done, over the definitions codegen compiles.

namespace sammine_lang::AST {

class NeverValueCheck : public ASTVisitor {
public:
  /// Generic templates aren't checked, only their copies.
  void visit(FuncDefAST *ast) override;

  void preorder_walk(CallExprAST *ast) override;
  void preorder_walk(BinaryExprAST *ast) override;
  void preorder_walk(UnaryNegExprAST *ast) override;
  void preorder_walk(DerefExprAST *ast) override;
  void preorder_walk(AddrOfExprAST *ast) override;
  void preorder_walk(AllocExprAST *ast) override;
  void preorder_walk(DropExprAST *ast) override;
  void preorder_walk(CastExprAST *ast) override;
  void preorder_walk(ArrayLiteralExprAST *ast) override;
  void preorder_walk(RangeExprAST *ast) override;
  void preorder_walk(IndexExprAST *ast) override;
  void preorder_walk(LenExprAST *ast) override;
  void preorder_walk(DimExprAST *ast) override;
  void preorder_walk(PanicExprAST *ast) override;
  void preorder_walk(StructLiteralExprAST *ast) override;
  void preorder_walk(FieldAccessExprAST *ast) override;
  void preorder_walk(TupleLiteralExprAST *ast) override;
  void preorder_walk(IfExprAST *ast) override;
  void preorder_walk(CaseExprAST *ast) override;
  void preorder_walk(WhileExprAST *ast) override;
  void preorder_walk(ForExprAST *ast) override;

private:
  /// Reports `used` if it never gives the value `as` says it is used for.
  void check(ExprAST *used, const char *as);
  void check(std::vector<std::unique_ptr<ExprAST>> &used, const char *as);
};

} // namespace sammine_lang::AST
