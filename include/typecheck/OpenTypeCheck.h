#pragma once
#include "ast/ASTProperties.h"
#include "ast/Ast.h"

//! \file OpenTypeCheck.h
//! \brief Reports each type argument nothing decided: a variant written
//! without the type arguments its payload doesn't decide, as None, whose
//! context never decided them either. Runs once type checking is done, over
//! the functions codegen compiles: nothing can decide them later, and MIR
//! has no type for an open one.

namespace sammine_lang::AST {

class OpenTypeCheck : public ASTVisitor {
public:
  explicit OpenTypeCheck(const ASTProperties &props) : props_(props) {}

  /// Generic templates aren't checked, only their copies; nor is a
  /// declaration, with no body.
  void visit(FuncDefAST *ast) override;
  void visit(TypeClassInstanceAST *ast) override;

  void preorder_walk(VariableExprAST *ast) override;
  void preorder_walk(CallExprAST *ast) override;

private:
  const ASTProperties &props_;

  /// Reports `expr`, the variant `variant`, if its type is still open.
  void check(const ExprAST *expr, const std::string &variant);
};

} // namespace sammine_lang::AST
