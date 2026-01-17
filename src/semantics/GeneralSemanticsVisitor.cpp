#include "semantics/GeneralSemanticsVisitor.h"
#include "ast/Ast.h"
#include "util/Utilities.h"
#include <memory>
//! \file GeneralSemanticsVisitor.cpp
//! \brief Implementation for GeneralSemanticsVisitor
namespace sammine_lang::AST {
void GeneralSemanticsVisitor::preorder_walk(FuncDefAST *ast) {
  // Track all function blocks, storing whether they return unit
  func_blocks[ast->Block.get()] = ast->returnsUnit();
}
void GeneralSemanticsVisitor::preorder_walk(BlockAST *ast) {
  // Only reset for function blocks, not nested blocks
  if (!func_blocks.contains(ast)) {
    returned = false;
  }
}
void GeneralSemanticsVisitor::postorder_walk(BlockAST *ast) {
  auto it = func_blocks.find(ast);
  if (it != func_blocks.end()) {
    // This is a function block
    if (!this->returned) {
      bool returns_unit = it->second;
      if (returns_unit) {
        // Unit-returning function: add implicit return unit
        ast->Statements.push_back(
            std::make_unique<ReturnExprAST>(std::make_unique<UnitExprAST>()));
      } else if (!ast->Statements.empty()) {
        // Value-returning function: wrap last statement in implicit return
        auto last_stmt = std::move(ast->Statements.back());
        // Only wrap if not already a return
        if (dynamic_cast<ReturnExprAST *>(last_stmt.get()) == nullptr) {
          ast->Statements.back() =
              std::make_unique<ReturnExprAST>(std::move(last_stmt));
        } else {
          // Put it back if it was already a return
          ast->Statements.back() = std::move(last_stmt);
        }
      }
    }
    // Only reset for function blocks to preserve returns from nested blocks
    returned = false;
  }
}
void GeneralSemanticsVisitor::preorder_walk(ReturnExprAST *ast) {
  returned = true;
}

} // namespace sammine_lang::AST
