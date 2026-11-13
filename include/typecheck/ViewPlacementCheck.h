#pragma once
#include "ast/Ast.h"

//! \file ViewPlacementCheck.h
//! \brief Where a view, a non-owning ptr<T>, may be: only the whole type of
//! a parameter or a local, so it never outlives what it points at. Never a
//! struct's field, an enum's payload, an array's element, a tuple's, a
//! return type, nor what owned memory holds (a view of a view is fine).
//! Each misplaced view is reported where its type is declared or made.
//! Runs once type checking is done, over the definitions codegen compiles.

namespace sammine_lang::AST {

class ViewPlacementCheck : public ASTVisitor {
public:
  void visit(StructDefAST *ast) override;
  void visit(EnumDefAST *ast) override;
  void visit(ExternAST *ast) override;
  /// Generic templates aren't checked, only their copies.
  void visit(FuncDefAST *ast) override;
  void visit(TypeClassInstanceAST *ast) override;

  void preorder_walk(VarDefAST *ast) override;
  void preorder_walk(StructLiteralExprAST *ast) override;
  void preorder_walk(ArrayLiteralExprAST *ast) override;
  void preorder_walk(TupleLiteralExprAST *ast) override;

private:
  /// Reports `type` if it holds a view where none may be. `ptr_allowed_here`
  /// says whether `type` itself may be one: so for a parameter or a local,
  /// not for a return or anything stored.
  void check(const Type &type, bool ptr_allowed_here,
             sammine_util::Location loc, const std::string &where);
  void check_prototype(PrototypeAST *proto);
};

} // namespace sammine_lang::AST
