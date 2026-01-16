#include "semantics/GeneralSemanticsVisitor.h"
#include "ast/Ast.h"
#include "util/Utilities.h"
#include <memory>
//! \file GeneralSemanticsVisitor.cpp
//! \brief Implementation for GeneralSemanticsVisitor
namespace sammine_lang::AST {
void GeneralSemanticsVisitor::preorder_walk(FuncDefAST *ast) {
  if (ast->returnsUnit())
    need_return.insert(ast->Block.get());
}
void GeneralSemanticsVisitor::preorder_walk(BlockAST *ast) { returned = false; }
void GeneralSemanticsVisitor::postorder_walk(BlockAST *ast) {
  if (!this->returned && need_return.contains(ast)) {
    ast->Statements.push_back(
        std::make_unique<ReturnExprAST>(std::make_unique<UnitExprAST>()));
  }
  returned = false;
}
void GeneralSemanticsVisitor::preorder_walk(ReturnExprAST *ast) {
  returned = true;
}

} // namespace sammine_lang::AST
