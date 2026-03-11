#pragma once
#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "util/LexicalContext.h"
#include "util/Utilities.h"
namespace sammine_lang::AST {

//! \file ScopeGeneratorVisitor.h
//! \brief Declares ScopeGeneratorVisitor, an ASTVisitor that builds and manages
//! lexical scope by registration and reporting if there's been redefinitions
class ScopeGeneratorVisitor : public ScopedASTVisitor {
public:
  using ScopedASTVisitor::visit;
  // A simple scoping class, doesn't differentiate between different names, like
  // variable name, func name and all that
  LexicalStack<sammine_util::Location> scope_stack;
  // variant_name → list of owning enum QualifiedNames
  std::map<std::string, std::vector<sammine_util::QualifiedName>> variant_to_enum;

  ScopeGeneratorVisitor() {
    scope_stack.push_context();
  }
  void qualify_type_expr(TypeExprAST *expr);

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
  NameQueryResult can_see(const std::string &symbol) const {
    return this->scope_stack.recursiveQueryName(symbol);
  }
  NameQueryResult can_see_parent(const std::string &symbol) const {
    const auto *parent_scope = this->scope_stack.parent_scope();
    return parent_scope->recursiveQueryName(symbol);
  }
  void register_name_parent(const std::string &symbol,
                            sammine_util::Location loc) {
    auto *parent_scope = this->scope_stack.parent_scope();
    return parent_scope->registerNameT(symbol, loc);
  }
  void register_name(const std::string &symbol, sammine_util::Location loc) {
    return this->scope_stack.registerNameT(symbol, loc);
  }
  // Case expressions: arm bodies contain pattern bindings not known at scope-gen time
  virtual void visit(CaseExprAST *ast) override {
    ast->scrutinee->accept_vis(this);
  }

  // Type class decls/instances: skip scope checking for their methods
  virtual void visit(TypeClassDeclAST *ast) override {}
  virtual void visit(TypeClassInstanceAST *ast) override {}

  virtual void visit(ExternAST *ast) override;

  // pre order — only non-empty overrides
  virtual void preorder_walk(ProgramAST *ast) override;
  virtual void preorder_walk(VarDefAST *ast) override;
  virtual void preorder_walk(ExternAST *ast) override;
  virtual void preorder_walk(FuncDefAST *ast) override;
  virtual void preorder_walk(PrototypeAST *ast) override;
  virtual void preorder_walk(CallExprAST *ast) override;
  virtual void preorder_walk(TypedVarAST *ast) override;
  virtual void preorder_walk(AllocExprAST *ast) override;

  // post order — only non-empty overrides
  virtual void postorder_walk(CallExprAST *ast) override;
  virtual void postorder_walk(VariableExprAST *ast) override;
  virtual void postorder_walk(StructLiteralExprAST *ast) override;
};

} // namespace sammine_lang::AST
