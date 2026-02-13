
#pragma once
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
  LexicalStack<sammine_util::Location, AST::FuncDefAST*> scope_stack;
  GeneralSemanticsVisitor() { scope_stack.push_context(); }

  // INFO: CheckAndReg means: Check if there's redefinition, if not, register
  // INFO: Check for castable means: Check if the name existed, if not, register

  virtual void enter_new_scope() override { this->scope_stack.push_context(); }
  virtual void exit_new_scope() override { this->scope_stack.pop_context(); }
  virtual void preorder_walk(FuncDefAST *ast) override;
  virtual void postorder_walk(BlockAST *ast) override;
  virtual void preorder_walk(BlockAST *ast) override;
  virtual void preorder_walk(ReturnExprAST *ast) override;

  virtual void preorder_walk(ProgramAST *ast) override {}

  virtual void preorder_walk(VarDefAST *ast) override {}

  virtual void preorder_walk(ExternAST *ast) override {}
  virtual void preorder_walk(RecordDefAST *ast) override {}
  virtual void preorder_walk(PrototypeAST *ast) override {}
  virtual void preorder_walk(CallExprAST *ast) override {}
  virtual void preorder_walk(BinaryExprAST *ast) override {}
  virtual void preorder_walk(NumberExprAST *ast) override {}
  virtual void preorder_walk(StringExprAST *ast) override {}
  virtual void preorder_walk(BoolExprAST *ast) override {}

  virtual void preorder_walk(VariableExprAST *ast) override {}
  virtual void preorder_walk(IfExprAST *ast) override {}
  virtual void preorder_walk(UnitExprAST *ast) override {}
  virtual void preorder_walk(TypedVarAST *ast) override {}
  virtual void preorder_walk(DerefExprAST *ast) override {}
  virtual void preorder_walk(AddrOfExprAST *ast) override {}
  virtual void preorder_walk(AllocExprAST *ast) override {}
  virtual void preorder_walk(FreeExprAST *ast) override {}

  // post order
  virtual void postorder_walk(ProgramAST *ast) override {}
  virtual void postorder_walk(VarDefAST *ast) override {}
  virtual void postorder_walk(ExternAST *ast) override {}
  virtual void postorder_walk(FuncDefAST *ast) override {}
  virtual void postorder_walk(RecordDefAST *ast) override {}
  virtual void postorder_walk(PrototypeAST *ast) override {}
  virtual void postorder_walk(CallExprAST *ast) override {}
  virtual void postorder_walk(ReturnExprAST *ast) override {}
  virtual void postorder_walk(BinaryExprAST *ast) override {}
  virtual void postorder_walk(NumberExprAST *ast) override {}
  virtual void postorder_walk(StringExprAST *ast) override {}
  virtual void postorder_walk(BoolExprAST *ast) override {}
  virtual void postorder_walk(VariableExprAST *ast) override {}
  virtual void postorder_walk(IfExprAST *ast) override {}
  virtual void postorder_walk(UnitExprAST *ast) override {}
  virtual void postorder_walk(TypedVarAST *ast) override {}
  virtual void postorder_walk(DerefExprAST *ast) override {}
  virtual void postorder_walk(AddrOfExprAST *ast) override {}
  virtual void postorder_walk(AllocExprAST *ast) override {}
  virtual void postorder_walk(FreeExprAST *ast) override {}
};
} // namespace sammine_lang::AST
