#pragma once
#include "ast/Ast.h"
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
  void consume(const std::string &name, sammine_util::Location loc);
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

public:
  void check(ProgramAST *program);
};

} // namespace sammine_lang::AST
