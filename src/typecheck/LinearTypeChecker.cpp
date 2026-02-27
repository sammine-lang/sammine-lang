#include "typecheck/LinearTypeChecker.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "typecheck/Types.h"

//! \file LinearTypeChecker.cpp
//! \brief Implements linear type checking — enforces that heap-allocated
//! ('ptr<T>) pointers are consumed exactly once before scope exit.

namespace sammine_lang::AST {

// ── Scope management ────────────────────────────────────────────────

void LinearTypeChecker::push_scope() { scope_stack.emplace_back(); }

void LinearTypeChecker::pop_scope_and_check(sammine_util::Location loc) {
  if (scope_stack.empty())
    return;
  auto &top = scope_stack.back();
  for (auto &[name, info] : top) {
    if (info.state == VarState::Unconsumed) {
      this->add_error(
          info.def_location,
          fmt::format(
              "Linear variable '{}' must be consumed before scope exit"
              " (use free() to deallocate)",
              name));
    }
  }
  scope_stack.pop_back();
}

// ── Variable tracking ───────────────────────────────────────────────

void LinearTypeChecker::register_linear(const std::string &name,
                                        sammine_util::Location loc) {
  if (scope_stack.empty())
    return;
  scope_stack.back()[name] = VarInfo{VarState::Unconsumed, loc, {}, name};
}

void LinearTypeChecker::consume(const std::string &name,
                                sammine_util::Location loc) {
  auto *info = find_linear(name);
  if (!info)
    return;
  if (info->state == VarState::Consumed) {
    this->add_error(
        loc,
        fmt::format("Linear variable '{}' has already been consumed", name));
    this->add_error(
        info->consume_location,
        fmt::format("'{}' was previously consumed here", name));
    return;
  }
  info->state = VarState::Consumed;
  info->consume_location = loc;
}

VarInfo *LinearTypeChecker::find_linear(const std::string &name) {
  for (auto it = scope_stack.rbegin(); it != scope_stack.rend(); ++it) {
    auto found = it->find(name);
    if (found != it->end())
      return &found->second;
  }
  return nullptr;
}

// ── Snapshot/restore for branch analysis ────────────────────────────

LinearVarMap LinearTypeChecker::snapshot() const {
  LinearVarMap merged;
  for (auto &scope : scope_stack) {
    for (auto &[name, info] : scope)
      merged[name] = info;
  }
  return merged;
}

void LinearTypeChecker::restore(const LinearVarMap &snap) {
  for (auto &scope : scope_stack) {
    for (auto &[name, info] : scope) {
      auto it = snap.find(name);
      if (it != snap.end())
        info = it->second;
    }
  }
}

// ── Branch consistency ──────────────────────────────────────────────

void LinearTypeChecker::check_branch_consistency(
    const LinearVarMap &before, const std::vector<LinearVarMap> &branches,
    sammine_util::Location loc) {
  // For each linear var that was Unconsumed before branches:
  // all branches must agree — either all consume it or none do.
  for (auto &[name, before_info] : before) {
    if (before_info.state != VarState::Unconsumed)
      continue;

    bool first_consumed = false;
    bool has_mismatch = false;

    for (size_t i = 0; i < branches.size(); i++) {
      auto it = branches[i].find(name);
      if (it == branches[i].end())
        continue;
      bool consumed = (it->second.state == VarState::Consumed);
      if (i == 0) {
        first_consumed = consumed;
      } else if (consumed != first_consumed) {
        has_mismatch = true;
        break;
      }
    }

    if (has_mismatch) {
      this->add_error(
          loc,
          fmt::format("Linear variable '{}' is consumed in some branches but "
                      "not others",
                      name));
      // Mark consumed to prevent cascading "must be consumed" at scope exit
      auto *info = find_linear(name);
      if (info)
        info->state = VarState::Consumed;
    } else if (first_consumed) {
      // All branches consumed it — apply directly (not via consume() to
      // avoid "already consumed" false positive)
      auto *info = find_linear(name);
      if (info)
        info->state = VarState::Consumed;
    }
  }
}

// ── Entry point ─────────────────────────────────────────────────────

void LinearTypeChecker::check(ProgramAST *program) {
  check_program(program);
}

void LinearTypeChecker::check_program(ProgramAST *ast) {
  for (auto &def : ast->DefinitionVec) {
    if (auto *func = llvm::dyn_cast<FuncDefAST>(def.get()))
      check_func(func);
  }
}

void LinearTypeChecker::check_func(FuncDefAST *ast) {
  push_scope();

  // Register linear parameters
  for (auto &param : ast->Prototype->parameterVectors) {
    if (param->type.is_linear)
      register_linear(param->name, param->get_location());
  }

  if (ast->Block)
    check_block(ast->Block.get());

  pop_scope_and_check(ast->get_location());
}

void LinearTypeChecker::check_block(BlockAST *ast) {
  for (auto &stmt : ast->Statements)
    check_stmt(stmt.get());
}

// ── Recursive dispatch ──────────────────────────────────────────────

void LinearTypeChecker::check_stmt(ExprAST *stmt) {
  if (auto *vd = llvm::dyn_cast<VarDefAST>(stmt))
    check_var_def(vd);
  else if (auto *bin = llvm::dyn_cast<BinaryExprAST>(stmt))
    check_binary(bin);
  else if (auto *call = llvm::dyn_cast<CallExprAST>(stmt))
    check_call(call);
  else if (auto *fr = llvm::dyn_cast<FreeExprAST>(stmt))
    check_free(fr);
  else if (auto *ret = llvm::dyn_cast<ReturnExprAST>(stmt))
    check_return(ret);
  else if (auto *iff = llvm::dyn_cast<IfExprAST>(stmt))
    check_if(iff);
  else if (auto *wh = llvm::dyn_cast<WhileExprAST>(stmt))
    check_while(wh);
  else if (auto *cs = llvm::dyn_cast<CaseExprAST>(stmt))
    check_case(cs);
  else if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(stmt))
    check_struct_literal(sl);
  else if (auto *al = llvm::dyn_cast<ArrayLiteralExprAST>(stmt))
    check_array_literal(al);
  // All other nodes (deref, number, string, etc.): no linear state changes
}

// ── check_var_def ───────────────────────────────────────────────────

void LinearTypeChecker::check_var_def(VarDefAST *ast) {
  if (!ast->Expression)
    return;

  // Check if RHS is a variable referencing a linear var (move)
  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->Expression.get())) {
    auto *info = find_linear(var->variableName);
    if (info) {
      consume(var->variableName, ast->get_location());
      if (ast->type.is_linear)
        register_linear(ast->TypedVar->name, ast->get_location());
      return;
    }
  }

  // Walk into RHS (may contain calls that consume things)
  check_stmt(ast->Expression.get());

  // If the variable itself is linear, register it
  if (ast->type.is_linear)
    register_linear(ast->TypedVar->name, ast->get_location());
}

// ── check_binary ────────────────────────────────────────────────────

void LinearTypeChecker::check_binary(BinaryExprAST *ast) {
  if (ast->Op->is_assign()) {
    // Assignment: check for move semantics on RHS
    if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->RHS.get())) {
      auto *rhs_info = find_linear(var->variableName);
      if (rhs_info) {
        // Check if LHS is a linear var being overwritten
        if (auto *lhs_var = llvm::dyn_cast<VariableExprAST>(ast->LHS.get())) {
          auto *lhs_info = find_linear(lhs_var->variableName);
          if (lhs_info && lhs_info->state == VarState::Unconsumed) {
            this->add_error(
                ast->get_location(),
                fmt::format("Reassigning linear variable '{}' without "
                            "consuming its previous value",
                            lhs_var->variableName));
            return;
          }
        }
        // Consume the RHS (move)
        consume(var->variableName, ast->get_location());
        // If LHS is a linear var, re-register it as unconsumed
        if (auto *lhs_var = llvm::dyn_cast<VariableExprAST>(ast->LHS.get())) {
          auto *lhs_info = find_linear(lhs_var->variableName);
          if (lhs_info) {
            lhs_info->state = VarState::Unconsumed;
            lhs_info->def_location = ast->get_location();
          }
        }
        return;
      }
    }
    // Check RHS for nested linear operations
    check_stmt(ast->RHS.get());
    // Check if LHS is a linear var being overwritten with alloc
    if (auto *lhs_var = llvm::dyn_cast<VariableExprAST>(ast->LHS.get())) {
      auto *lhs_info = find_linear(lhs_var->variableName);
      if (lhs_info && lhs_info->state == VarState::Unconsumed) {
        if (ast->RHS->type.is_linear) {
          this->add_error(
              ast->get_location(),
              fmt::format("Reassigning linear variable '{}' without "
                          "consuming its previous value",
                          lhs_var->variableName));
          return;
        }
      }
      // Re-register if RHS is linear (e.g. q = alloc<i32>(1))
      if (lhs_info && ast->RHS->type.is_linear) {
        lhs_info->state = VarState::Unconsumed;
        lhs_info->def_location = ast->get_location();
      }
    }
  } else {
    // Non-assignment binary: walk both sides
    check_stmt(ast->LHS.get());
    check_stmt(ast->RHS.get());
  }
}

// ── check_call ──────────────────────────────────────────────────────

void LinearTypeChecker::check_call(CallExprAST *ast) {
  // Check each argument: if it's a variable referencing a linear var,
  // and the parameter type is linear, consume it.
  if (ast->callee_func_type) {
    auto &func_type = std::get<FunctionType>(ast->callee_func_type->type_data);
    auto params = func_type.get_params_types();
    for (size_t i = 0; i < ast->arguments.size() && i < params.size(); i++) {
      if (auto *var =
              llvm::dyn_cast<VariableExprAST>(ast->arguments[i].get())) {
        auto *info = find_linear(var->variableName);
        if (info && params[i].is_linear) {
          consume(var->variableName, ast->get_location());
          continue;
        }
      }
      // Walk into nested expressions
      check_stmt(ast->arguments[i].get());
    }
  } else {
    // No callee type info — walk arguments for nested linear ops
    for (auto &arg : ast->arguments)
      check_stmt(arg.get());
  }
}

// ── check_free ──────────────────────────────────────────────────────

void LinearTypeChecker::check_free(FreeExprAST *ast) {
  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->operand.get())) {
    auto *info = find_linear(var->variableName);
    if (info) {
      consume(var->variableName, ast->get_location());
      return;
    }
  }
  // Walk operand for nested linear ops
  check_stmt(ast->operand.get());
}

// ── check_return ────────────────────────────────────────────────────

void LinearTypeChecker::check_return(ReturnExprAST *ast) {
  if (!ast->return_expr)
    return;

  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->return_expr.get())) {
    auto *info = find_linear(var->variableName);
    if (info) {
      // Returning a linear pointer: ownership transfers to caller
      consume(var->variableName, ast->get_location());
      return;
    }
    // Returning a non-linear pointer — error (may dangle)
    if (var->type.type_kind == TypeKind::Pointer && !var->type.is_linear) {
      this->add_error(
          ast->get_location(),
          "Cannot return non-linear pointer (may reference a local variable)");
      return;
    }
  }

  // Walk into return expression
  check_stmt(ast->return_expr.get());
}

// ── check_if ────────────────────────────────────────────────────────

void LinearTypeChecker::check_if(IfExprAST *ast) {
  // Condition can consume (unconditional)
  check_stmt(ast->bool_expr.get());

  auto before = snapshot();

  // Then branch
  check_block(ast->thenBlockAST.get());
  auto state_then = snapshot();

  // Restore for else
  restore(before);

  LinearVarMap state_else;
  if (ast->elseBlockAST) {
    check_block(ast->elseBlockAST.get());
    state_else = snapshot();
  } else {
    // No else = "no change" branch
    state_else = before;
  }

  // Restore before state, then apply consistency results
  restore(before);
  std::vector<LinearVarMap> branches = {state_then, state_else};
  check_branch_consistency(before, branches, ast->get_location());
}

// ── check_while ─────────────────────────────────────────────────────

void LinearTypeChecker::check_while(WhileExprAST *ast) {
  auto before = snapshot();
  loop_depth++;

  check_stmt(ast->condition.get());
  check_block(ast->body.get());

  auto after = snapshot();
  loop_depth--;

  // Check that no outer linear vars were consumed inside the loop
  for (auto &[name, before_info] : before) {
    if (before_info.state != VarState::Unconsumed)
      continue;
    auto it = after.find(name);
    if (it != after.end() && it->second.state == VarState::Consumed) {
      this->add_error(
          it->second.consume_location,
          fmt::format(
              "Cannot consume linear variable '{}' inside a loop"
              " (defined outside the loop)",
              name));
    }
  }

  // Restore state (loop might run 0 times, so outer state is unchanged)
  restore(before);
}

// ── check_case ──────────────────────────────────────────────────────

void LinearTypeChecker::check_case(CaseExprAST *ast) {
  // Walk scrutinee
  check_stmt(ast->scrutinee.get());

  auto before = snapshot();
  std::vector<LinearVarMap> arm_states;

  for (auto &arm : ast->arms) {
    restore(before);
    if (arm.body)
      check_block(arm.body.get());
    arm_states.push_back(snapshot());
  }

  restore(before);
  check_branch_consistency(before, arm_states, ast->get_location());
}

// ── check_struct_literal ─────────────────────────────────────────────

void LinearTypeChecker::check_struct_literal(StructLiteralExprAST *ast) {
  for (auto &field_val : ast->field_values) {
    if (auto *var = llvm::dyn_cast<VariableExprAST>(field_val.get())) {
      auto *info = find_linear(var->variableName);
      if (info) {
        consume(var->variableName, ast->get_location());
        continue;
      }
    }
    check_stmt(field_val.get());
  }
}

// ── check_array_literal ─────────────────────────────────────────────

void LinearTypeChecker::check_array_literal(ArrayLiteralExprAST *ast) {
  for (auto &elem : ast->elements) {
    if (auto *var = llvm::dyn_cast<VariableExprAST>(elem.get())) {
      auto *info = find_linear(var->variableName);
      if (info) {
        consume(var->variableName, ast->get_location());
        continue;
      }
    }
    check_stmt(elem.get());
  }
}

} // namespace sammine_lang::AST
