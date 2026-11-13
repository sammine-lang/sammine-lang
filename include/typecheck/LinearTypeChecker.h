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

  // Reaching memory through a pointer: only an owner may write, and a plain
  // pointer into owned memory would still be readable after free.
  void check_through_pointer(ExprAST *pointer, Use use,
                             sammine_util::Location loc);
  void check_var_def(VarDefAST *ast);
  void check_store(BinaryExprAST *ast);
  void check_call(CallExprAST *ast);
  void check_returned_value(ReturnStmtAST *ast);
  void check_case(CaseExprAST *ast);
  void check_addr_of(AddrOfExprAST *ast);

  // Report `type` if it holds a non-owning ptr<T> where one is not allowed.
  // `ptr_allowed_here` says whether `type` itself may be a ptr<T>: true for
  // parameters and locals, false for returns and anything stored.
  void check_placement(const Type &type, bool ptr_allowed_here,
                       sammine_util::Location loc, const std::string &where);
  void check_prototype_placement(PrototypeAST *proto);

  // Reject a wildcard arm that could match a variant with a linear payload.
  void check_wildcard_arm(const CaseExprAST &ast, const CaseArm &wildcard);

  const ASTProperties *props_ = nullptr;

  // Maps closure variable names to the types of their captured (bound) args.
  // Used at return time: if any captured type containsNonLinearPtr(), the
  // closure cannot escape because it holds a reference to stack-local data.
  std::unordered_map<std::string, std::vector<Type>> closure_captures_;

  // Record the bound arg types of a partial application into closure_captures_.
  void record_closure_captures(CallExprAST *ast, const std::string &dest_var);

public:
  void check(ProgramAST *program, const ASTProperties &props);
};

} // namespace sammine_lang::AST
