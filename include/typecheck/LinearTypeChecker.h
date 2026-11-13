#pragma once
#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "util/Utilities.h"
#include <string>
#include <unordered_map>
#include <vector>

//! \file LinearTypeChecker.h
//! \brief The rules on pointers and linear values that don't depend on
//! control flow: where a non-owning ptr<T> may appear, what may be written
//! through or have its address taken, what a closure may bind or a function
//! return. Whether each linear value is consumed exactly once is checked on
//! MIR (dialect/OwnershipCheck.h).
//!
//! Runs as a separate pass after BiTypeChecker.

namespace sammine_lang::AST {

class LinearTypeChecker : public sammine_util::Reportee {
  // What a parent expression does with what a child gives it: takes it,
  // reads it, or, for a place, writes it or takes its address.
  enum class Use { Own, Read, Write, TakeAddress };

  void check_program(ProgramAST *ast);
  void check_func(FuncDefAST *ast);
  // A block's last statement is its value: owned if `result_type` is linear.
  void check_block(BlockAST *ast, const Type &result_type);
  void check_expr(ExprAST *expr, Use use = Use::Read);

  void check_var_def(VarDefAST *ast);
  void check_store(BinaryExprAST *ast);
  void check_call(CallExprAST *ast);
  // `value` returned, by a return at `loc` or by a body running off its end.
  void check_returned_value(ExprAST *value, sammine_util::Location loc);
  void check_case(CaseExprAST *ast);

  // Reject a wildcard arm that could match a variant with a linear payload.
  void check_wildcard_arm(const CaseExprAST &ast, const CaseArm &wildcard);

  const ASTProperties *props_ = nullptr;

public:
  void check(ProgramAST *program, const ASTProperties &props);
};

} // namespace sammine_lang::AST
