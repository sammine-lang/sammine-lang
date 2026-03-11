
#pragma once
#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "ast/AstDecl.h"
#include "util/LexicalContext.h"
#include "util/Utilities.h"
#include <map>
//! \file GeneralSemanticsVisitor.h
//! \brief Defines GeneralSemanticsVisitor, an ASTVisitor that enforces general
//! semantic rules for scoped definitions, detecting duplicates,
//! and ensuring correct return usage in blocks.
namespace sammine_lang::AST {
/// General
class GeneralSemanticsVisitor : public ScopedASTVisitor {
  // Maps function blocks to whether they return unit (true) or a value (false)
  std::map<BlockAST *, bool> func_blocks;
  bool returned = false;

public:
  // A simple scoping class, doesn't differentiate between different names, like
  // variable name, func name and all that
  LexicalStack<sammine_util::Location> scope_stack;
  GeneralSemanticsVisitor() { scope_stack.push_context(); }

  // Check if name contains "__" (reserved for C mangling)
  void check_reserved_identifier(const std::string &name,
                                 sammine_util::Location loc);

  // INFO: CheckAndReg means: Check if there's redefinition, if not, register
  // INFO: Check for castable means: Check if the name existed, if not, register

  virtual void enter_new_scope() override {
    push_ast_context();
    this->scope_stack.push_context();
  }
  virtual void exit_new_scope() override {
    this->scope_stack.pop_context();
    pop_ast_context();
  }
  // Only non-empty overrides
  virtual void preorder_walk(FuncDefAST *ast) override;
  virtual void preorder_walk(BlockAST *ast) override;
  virtual void preorder_walk(ReturnExprAST *ast) override;
  virtual void preorder_walk(VarDefAST *ast) override;
  virtual void preorder_walk(StructDefAST *ast) override;
  virtual void preorder_walk(EnumDefAST *ast) override;
  virtual void preorder_walk(TypeAliasDefAST *ast) override;

  virtual void postorder_walk(BlockAST *ast) override;

  using ScopedASTVisitor::visit;
  virtual void visit(TypeClassDeclAST *ast) override {}
  virtual void visit(TypeClassInstanceAST *ast) override {
    for (auto &method : ast->methods)
      method->accept_vis(this);
  }
};
} // namespace sammine_lang::AST
