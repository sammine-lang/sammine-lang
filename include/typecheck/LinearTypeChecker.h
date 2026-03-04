#pragma once
#include "ast/Ast.h"
#include "ast/ASTProperties.h"
#include "util/Utilities.h"
#include <string>
#include <unordered_map>
#include <vector>

//! \file LinearTypeChecker.h
//! \brief Linear type checker — tracks heap-allocated pointers and enforces
//! they are consumed (freed or moved) exactly once before scope exit.
//!
//! Uses direct recursive dispatch via dyn_cast (no visitor pattern).
//! Runs as a separate pass after BiTypeChecker.

namespace sammine_lang::AST {

enum class VarState { Unconsumed, Consumed };

struct VarInfo {
  VarState state;
  sammine_util::Location def_location;
  sammine_util::Location consume_location;
  std::string name;
  std::string consume_reason;
  // Recursive: per-field/element tracking for wrapper types with linear innards
  std::unordered_map<std::string, VarInfo> children;
};

using LinearVarMap = std::unordered_map<std::string, VarInfo>;

class LinearTypeChecker : public sammine_util::Reportee {
  std::vector<LinearVarMap> scope_stack;
  int loop_depth = 0;

  // Scope management
  void push_scope();
  void pop_scope_and_check(sammine_util::Location loc);

  // Variable tracking
  void register_linear(const std::string &name, sammine_util::Location loc);
  void consume(VarInfo *info, sammine_util::Location loc,
               const std::string &reason = "consumed");
  VarInfo *find_linear(const std::string &name);

  // Get merged view of all scopes for snapshotting
  LinearVarMap snapshot() const;
  void restore(const LinearVarMap &snap);

  // Branch analysis
  void check_branch_consistency(const LinearVarMap &before,
                                const std::vector<LinearVarMap> &branches,
                                sammine_util::Location loc);

  // Recursive dispatch
  void check_program(ProgramAST *ast);
  void check_func(FuncDefAST *ast);
  void check_block(BlockAST *ast);
  void check_stmt(ExprAST *stmt);
  void check_var_def(VarDefAST *ast);
  void check_binary(BinaryExprAST *ast);
  void check_call(CallExprAST *ast);
  void check_free(FreeExprAST *ast);
  void check_return(ReturnExprAST *ast);
  void check_if(IfExprAST *ast);
  void check_while(WhileExprAST *ast);
  void check_case(CaseExprAST *ast);
  void check_addr_of(AddrOfExprAST *ast);
  void check_deref(DerefExprAST *ast);
  void check_index(IndexExprAST *ast);
  void check_struct_literal(StructLiteralExprAST *ast);
  void check_array_literal(ArrayLiteralExprAST *ast);
  void check_tuple_literal(TupleLiteralExprAST *ast);

  // Recursive inner-linear tracking for wrapper types
  void register_inner_linear(VarInfo &parent, const Type &t,
                              sammine_util::Location loc);
  void check_children_consumed(const VarInfo &info);
  VarInfo *find_child(const std::string &var_name,
                       const std::string &field_name);

  const ASTProperties *props_ = nullptr;

public:
  void check(ProgramAST *program, const ASTProperties &props);
};

} // namespace sammine_lang::AST
