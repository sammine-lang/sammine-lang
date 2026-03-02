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
  LexicalStack<sammine_util::Location, AST::FuncDefAST *> scope_stack;
  // variant_name → list of owning enum QualifiedNames
  std::map<std::string, std::vector<sammine_util::QualifiedName>> variant_to_enum;
  // Track when we're inside an imported definition whose type expressions
  // need module-qualifying (imported generic functions and imported externs)
  bool insideImportedDef_ = false;
  std::string currentImportModule_;
  // Type params of the current imported generic function (skip qualification)
  std::vector<std::string> currentGenericTypeParams_;
  ScopeGeneratorVisitor() {
    scope_stack.push_context();
  }
  void qualify_type_expr(TypeExprAST *expr);

  // INFO: CheckAndReg means: Check if there's redefinition, if not, register
  // INFO: Check for castable means: Check if the name existed, if not, register

  virtual void enter_new_scope() override { this->scope_stack.push_context(); }
  virtual void exit_new_scope() override { this->scope_stack.pop_context(); }
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
  // pre order

  // INFO: Nothing here
  virtual void preorder_walk(ProgramAST *ast) override;

  // INFO: CheckAndReg variable name
  virtual void preorder_walk(VarDefAST *ast) override;

  // Case expressions: arm bodies contain pattern bindings not known at scope-gen time
  virtual void visit(CaseExprAST *ast) override {
    ast->scrutinee->accept_vis(this);
  }

  // Type class decls/instances: skip scope checking for their methods
  virtual void visit(TypeClassDeclAST *ast) override {}
  virtual void visit(TypeClassInstanceAST *ast) override {}

  // INFO: CheckAndReg extern name
  virtual void visit(ExternAST *ast) override;
  virtual void preorder_walk(ExternAST *ast) override;
  // INFO: CheckAndReg function name, enter new block
  virtual void preorder_walk(FuncDefAST *ast) override;
  virtual void preorder_walk(StructDefAST *ast) override;
  virtual void preorder_walk(EnumDefAST *ast) override;
  virtual void preorder_walk(TypeAliasDefAST *ast) override;
  // INFO: CheckAndReg all variable name, which should only clash if you have
  // the same names in prototype
  virtual void preorder_walk(PrototypeAST *ast) override;
  // INFO: Check
  virtual void preorder_walk(CallExprAST *ast) override;
  virtual void preorder_walk(ReturnExprAST *ast) override;
  virtual void preorder_walk(BinaryExprAST *ast) override;
  virtual void preorder_walk(NumberExprAST *ast) override;
  virtual void preorder_walk(StringExprAST *ast) override;
  virtual void preorder_walk(BoolExprAST *ast) override;
  virtual void preorder_walk(CharExprAST *ast) override;

  // INFO: Check
  virtual void preorder_walk(VariableExprAST *ast) override;
  virtual void preorder_walk(BlockAST *ast) override;
  virtual void preorder_walk(IfExprAST *ast) override;
  virtual void preorder_walk(UnitExprAST *ast) override;
  virtual void preorder_walk(TypedVarAST *ast) override;
  virtual void preorder_walk(DerefExprAST *ast) override;
  virtual void preorder_walk(AddrOfExprAST *ast) override;
  virtual void preorder_walk(AllocExprAST *ast) override;
  virtual void preorder_walk(FreeExprAST *ast) override;
  virtual void preorder_walk(ArrayLiteralExprAST *ast) override;
  virtual void preorder_walk(IndexExprAST *ast) override;
  virtual void preorder_walk(LenExprAST *ast) override;
  virtual void preorder_walk(UnaryNegExprAST *ast) override;
  virtual void preorder_walk(StructLiteralExprAST *ast) override;
  virtual void preorder_walk(FieldAccessExprAST *ast) override;
  virtual void preorder_walk(CaseExprAST *ast) override;
  virtual void preorder_walk(WhileExprAST *ast) override;
  virtual void preorder_walk(TupleLiteralExprAST *ast) override;
  virtual void preorder_walk(TypeClassDeclAST *ast) override;
  virtual void preorder_walk(TypeClassInstanceAST *ast) override;

  // post order
  virtual void postorder_walk(ProgramAST *ast) override;
  virtual void postorder_walk(VarDefAST *ast) override;
  virtual void postorder_walk(ExternAST *ast) override;
  // INFO: Pop the scope
  virtual void postorder_walk(FuncDefAST *ast) override;
  virtual void postorder_walk(StructDefAST *ast) override;
  virtual void postorder_walk(EnumDefAST *ast) override;
  virtual void postorder_walk(TypeAliasDefAST *ast) override;
  virtual void postorder_walk(PrototypeAST *ast) override;
  virtual void postorder_walk(CallExprAST *ast) override;
  virtual void postorder_walk(ReturnExprAST *ast) override;
  virtual void postorder_walk(BinaryExprAST *ast) override;
  virtual void postorder_walk(NumberExprAST *ast) override;
  virtual void postorder_walk(StringExprAST *ast) override;
  virtual void postorder_walk(BoolExprAST *ast) override;
  virtual void postorder_walk(CharExprAST *ast) override;
  virtual void postorder_walk(VariableExprAST *ast) override;
  virtual void postorder_walk(BlockAST *ast) override;
  virtual void postorder_walk(IfExprAST *ast) override;
  virtual void postorder_walk(UnitExprAST *ast) override;
  virtual void postorder_walk(TypedVarAST *ast) override;
  virtual void postorder_walk(DerefExprAST *ast) override;
  virtual void postorder_walk(AddrOfExprAST *ast) override;
  virtual void postorder_walk(AllocExprAST *ast) override;
  virtual void postorder_walk(FreeExprAST *ast) override;
  virtual void postorder_walk(ArrayLiteralExprAST *ast) override;
  virtual void postorder_walk(IndexExprAST *ast) override;
  virtual void postorder_walk(LenExprAST *ast) override;
  virtual void postorder_walk(UnaryNegExprAST *ast) override;
  virtual void postorder_walk(StructLiteralExprAST *ast) override;
  virtual void postorder_walk(FieldAccessExprAST *ast) override;
  virtual void postorder_walk(CaseExprAST *ast) override;
  virtual void postorder_walk(WhileExprAST *ast) override;
  virtual void postorder_walk(TupleLiteralExprAST *ast) override;
  virtual void postorder_walk(TypeClassDeclAST *ast) override;
  virtual void postorder_walk(TypeClassInstanceAST *ast) override;
};

} // namespace sammine_lang::AST
