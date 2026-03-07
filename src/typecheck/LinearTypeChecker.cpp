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
      // Check children first (struct/array/tuple with linear fields)
      if (!info.children.empty()) {
        check_children_consumed(info);
      } else {
        this->add_error(
            info.def_location,
            fmt::format(
                "Linear variable '{}' must be consumed before scope exit"
                " (use free() to deallocate)",
                name));
      }
    }
  }
  scope_stack.pop_back();
}

// ── Variable tracking ───────────────────────────────────────────────

void LinearTypeChecker::register_linear(const std::string &name,
                                        sammine_util::Location loc) {
  if (scope_stack.empty())
    return;
  scope_stack.back()[name] = VarInfo{VarState::Unconsumed, loc, {}, name, {}, {}};
}

void LinearTypeChecker::consume(VarInfo *info, sammine_util::Location loc,
                                const std::string &reason) {
  if (info->state == VarState::Consumed) {
    this->add_error(
        loc, fmt::format("Cannot use linear variable '{}' — it was already {} previously",
                         info->name, info->consume_reason));
    this->add_error(
        info->consume_location,
        fmt::format("'{}' was {} here", info->name, info->consume_reason));
    return;
  }
  info->state = VarState::Consumed;
  info->consume_location = loc;
  info->consume_reason = reason;
}

bool LinearTypeChecker::check_use_after_consume(VarInfo *info,
                                                 sammine_util::Location use_loc,
                                                 const std::string &verb) {
  if (!info || info->state != VarState::Consumed)
    return false;
  this->add_error(
      use_loc,
      fmt::format("Cannot {} '{}' — it was already {} previously",
                  verb, info->name, info->consume_reason));
  this->add_error(
      info->consume_location,
      fmt::format("'{}' was {} here", info->name, info->consume_reason));
  return true;
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

LinearTypeChecker::BranchAgreement LinearTypeChecker::check_agreement(
    const std::vector<LinearVarMap> &branches,
    const StateExtractor &get_state) {
  bool first_consumed = false;
  bool seen_first = false;
  for (auto &branch : branches) {
    auto state = get_state(branch);
    if (!state)
      continue;
    bool consumed = (*state == VarState::Consumed);
    if (!seen_first) {
      first_consumed = consumed;
      seen_first = true;
    } else if (consumed != first_consumed) {
      return BranchAgreement::Mismatch;
    }
  }
  return (seen_first && first_consumed) ? BranchAgreement::AllConsumed
                                        : BranchAgreement::AllUnconsumed;
}

void LinearTypeChecker::apply_consistency(BranchAgreement result, VarInfo *info,
                                          const std::string &display_name,
                                          sammine_util::Location loc) {
  if (result == BranchAgreement::Mismatch) {
    this->add_error(
        loc, fmt::format("Linear variable '{}' is consumed in some branches "
                         "but not others",
                         display_name));
    if (info)
      info->state = VarState::Consumed;
  } else if (result == BranchAgreement::AllConsumed && info) {
    info->state = VarState::Consumed;
  }
}

void LinearTypeChecker::check_branch_consistency(
    const LinearVarMap &before, const std::vector<LinearVarMap> &branches,
    sammine_util::Location loc) {
  for (auto &[name, before_info] : before) {
    if (before_info.state != VarState::Unconsumed)
      continue;

    auto *info = find_linear(name);

    // Check the var itself
    auto result = check_agreement(branches, [&](const LinearVarMap &b)
        -> std::optional<VarState> {
      auto it = b.find(name);
      return it != b.end() ? std::optional(it->second.state) : std::nullopt;
    });
    apply_consistency(result, info, name, loc);

    // Check children (e.g. v.data inside a struct wrapper)
    for (auto &[child_name, child_before] : before_info.children) {
      if (child_before.state != VarState::Unconsumed)
        continue;

      auto child_result = check_agreement(branches, [&](const LinearVarMap &b)
          -> std::optional<VarState> {
        auto it = b.find(name);
        if (it == b.end()) return std::nullopt;
        auto cit = it->second.children.find(child_name);
        return cit != it->second.children.end()
            ? std::optional(cit->second.state) : std::nullopt;
      });
      apply_consistency(child_result,
                        info ? find_child(name, child_name) : nullptr,
                        child_before.name, loc);
    }
  }
}

// ── Entry point ─────────────────────────────────────────────────────

void LinearTypeChecker::check(ProgramAST *program, const ASTProperties &props) {
  props_ = &props;
  check_program(program);
}

void LinearTypeChecker::check_program(ProgramAST *ast) {
  for (auto &def : ast->DefinitionVec) {
    if (auto *func = llvm::dyn_cast<FuncDefAST>(def.get()))
      check_func(func);
  }
}

void LinearTypeChecker::check_func(FuncDefAST *ast) {
  // Skip generic function templates — only check monomorphized copies.
  if (ast->Prototype->is_generic())
    return;

  push_scope();

  // Register linear parameters (including wrapper types with linear fields)
  for (auto &param : ast->Prototype->parameterVectors)
    register_if_linear(param->name, param->get_type(), param->get_location());

  if (ast->Block)
    check_block(ast->Block.get());

  pop_scope_and_check(ast->get_location());
}

void LinearTypeChecker::check_block(BlockAST *ast) {
  for (auto &stmt : ast->Statements) {
    check_stmt(stmt.get());

    // Check for bare statements that discard linear values
    if (llvm::isa<AllocExprAST>(stmt.get())) {
      this->add_error(stmt->get_location(),
                      "Linear pointer from alloc is discarded"
                      " (must be stored in a variable)");
    } else if (auto *call = llvm::dyn_cast<CallExprAST>(stmt.get())) {
      if (call->get_type().containsLinearTypes()) {
        this->add_error(
            call->get_location(),
            fmt::format(
                "Return value of '{}' has linear type '{}' and must not be "
                "discarded",
                call->functionName.mangled(), call->get_type().to_string()));
      }
    }
  }
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
  else if (auto *tl = llvm::dyn_cast<TupleLiteralExprAST>(stmt))
    check_tuple_literal(tl);
  else if (auto *dr = llvm::dyn_cast<DerefExprAST>(stmt))
    check_deref(dr);
  else if (auto *idx = llvm::dyn_cast<IndexExprAST>(stmt))
    check_index(idx);
  else if (auto *addr = llvm::dyn_cast<AddrOfExprAST>(stmt))
    check_addr_of(addr);
  // All other nodes (number, string, etc.): no linear state changes
}

// ── check_var_def ───────────────────────────────────────────────────

void LinearTypeChecker::check_var_def(VarDefAST *ast) {
  if (!ast->Expression)
    return;

  auto loc = ast->get_location();

  // Tuple destructuring: let (a, b) = expr;
  if (ast->is_tuple_destructure) {
    // If RHS is a wrapper var, consume it (children dissolve into individual vars)
    if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->Expression.get())) {
      auto *info = find_linear(var->variableName);
      if (info)
        consume(info, loc, "destructured");
    }
    // Walk into RHS for other expressions
    check_stmt(ast->Expression.get());
    // Register any linear vars from the destructured elements
    for (auto &var : ast->destructure_vars) {
      if (var->get_type().linearity == Linearity::Linear)
        register_linear(var->name, var->get_location());
    }
    return;
  }

  // Check if RHS is a variable referencing a linear var (move)
  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->Expression.get())) {
    auto *info = find_linear(var->variableName);
    if (info) {
      consume(info, loc, "moved");
      register_if_linear(ast->TypedVar->name, ast->get_type(), loc);
      return;
    }
    // Propagate closure captures through variable-to-variable assignment
    auto it = closure_captures_.find(var->variableName);
    if (it != closure_captures_.end())
      closure_captures_[ast->TypedVar->name] = it->second;
  }

  // Check if RHS is a field access extracting a linear field (e.g. let q = b.data)
  if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(ast->Expression.get())) {
    if (auto *obj = llvm::dyn_cast<VariableExprAST>(fa->object_expr.get())) {
      auto *child = find_child(obj->variableName, fa->field_name);
      if (child) {
        consume(child, loc, "moved");
        register_if_linear(ast->TypedVar->name, ast->get_type(), loc);
        return;
      }
    }
  }

  // Record captured types if RHS is a partial application
  if (auto *call = llvm::dyn_cast<CallExprAST>(ast->Expression.get()))
    record_closure_captures(call, ast->TypedVar->name);

  // Propagate closure captures through compound literals (struct/tuple/array).
  // If any element is a closure that captured a non-linear pointer,
  // the enclosing variable inherits that taint.
  auto propagate_from_elements =
      [&](const std::vector<std::unique_ptr<ExprAST>> &elements) {
        for (auto &elem : elements) {
          if (auto *v = llvm::dyn_cast<VariableExprAST>(elem.get())) {
            auto it = closure_captures_.find(v->variableName);
            if (it != closure_captures_.end()) {
              closure_captures_[ast->TypedVar->name] = it->second;
              return;
            }
          }
        }
      };
  if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(ast->Expression.get()))
    propagate_from_elements(sl->field_values);
  else if (auto *tl = llvm::dyn_cast<TupleLiteralExprAST>(ast->Expression.get()))
    propagate_from_elements(tl->elements);
  else if (auto *al = llvm::dyn_cast<ArrayLiteralExprAST>(ast->Expression.get()))
    propagate_from_elements(al->elements);

  // Walk into RHS (may contain calls that consume things)
  check_stmt(ast->Expression.get());

  register_if_linear(ast->TypedVar->name, ast->get_type(), loc);
}

// ── Non-linear pointer path detection ───────────────────────────────

// Returns a human-readable path to the first non-linear pointer found,
// e.g. "non-linear ptr<i32> in field 'data'", or nullopt if none.
// NOTE: if you add a new wrapping TypeKind to forEachInnerType, add it here too.
static std::optional<std::string> find_nonlinear_pointer_path(const Type &t) {
  switch (t.type_kind) {
  case TypeKind::Pointer:
    if (t.linearity != Linearity::Linear)
      return fmt::format("non-linear {}", t.to_string());
    return std::nullopt;
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(t.type_data);
    auto &names = st.get_field_names();
    auto &types = st.get_field_types();
    for (size_t i = 0; i < types.size(); i++) {
      auto path = find_nonlinear_pointer_path(types[i]);
      if (path)
        return fmt::format("{} in field '{}'", *path, names[i]);
    }
    return std::nullopt;
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(t.type_data);
    for (auto &variant : et.get_variants()) {
      for (auto &pt : variant.payload_types) {
        auto path = find_nonlinear_pointer_path(pt);
        if (path)
          return fmt::format("{} in variant '{}'", *path, variant.name);
      }
    }
    return std::nullopt;
  }
  case TypeKind::Array: {
    auto &at = std::get<ArrayType>(t.type_data);
    auto path = find_nonlinear_pointer_path(at.get_element());
    if (path)
      return fmt::format("{} in element", *path);
    return std::nullopt;
  }
  case TypeKind::Tuple: {
    auto &tt = std::get<TupleType>(t.type_data);
    for (size_t i = 0; i < tt.size(); i++) {
      auto path = find_nonlinear_pointer_path(tt.get_element(i));
      if (path)
        return fmt::format("{} in tuple element {}", *path, i);
    }
    return std::nullopt;
  }
  case TypeKind::Function: {
    auto &fn = std::get<FunctionType>(t.type_data);
    for (auto &p : fn.get_params_types()) {
      auto path = find_nonlinear_pointer_path(p);
      if (path)
        return fmt::format("{} in parameter", *path);
    }
    auto path = find_nonlinear_pointer_path(fn.get_return_type());
    if (path)
      return fmt::format("{} in return type", *path);
    return std::nullopt;
  }
  case TypeKind::I32_t:
  case TypeKind::I64_t:
  case TypeKind::U32_t:
  case TypeKind::U64_t:
  case TypeKind::F64_t:
  case TypeKind::F32_t:
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::Char:
  case TypeKind::String:
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::Integer:
  case TypeKind::Flt:
  case TypeKind::TypeParam:
  case TypeKind::Generic:
    return std::nullopt;
  }
}

// ── Lateral escape detection ────────────────────────────────────────

// Returns true if the expression involves a pointer dereference in its
// access chain.  Used to detect assignments like (*p).field = &local
// where a non-linear pointer could escape through indirection.
static bool involves_deref(ExprAST *expr) {
  if (llvm::isa<DerefExprAST>(expr))
    return true;
  if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(expr))
    return involves_deref(fa->object_expr.get());
  if (auto *idx = llvm::dyn_cast<IndexExprAST>(expr))
    return involves_deref(idx->array_expr.get());
  return false;
}

// ── check_binary ────────────────────────────────────────────────────

void LinearTypeChecker::check_binary(BinaryExprAST *ast) {
  if (ast->Op->is_assign()) {
    // Check LHS for deref-after-consume (e.g. free(p); *p = 7)
    check_stmt(ast->LHS.get());

    // Lateral escape: storing a non-linear pointer through a pointer
    // dereference allows it to outlive the current scope.
    if (involves_deref(ast->LHS.get())) {
      auto path = find_nonlinear_pointer_path(ast->RHS->get_type());
      if (path) {
        this->add_error(
            ast->get_location(),
            fmt::format("Cannot store {} through pointer dereference"
                        " — may dangle after scope exit",
                        *path));
        return;
      }
    }

    // RHS: linear variable → consume (move), otherwise walk for nested ops
    if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->RHS.get())) {
      auto *rhs_info = find_linear(var->variableName);
      if (rhs_info)
        consume(rhs_info, ast->get_location(), "moved");
      else
        check_stmt(ast->RHS.get());
    } else {
      check_stmt(ast->RHS.get());
    }

    // LHS deref: *pp = x updates the "*" child's location
    if (auto *deref = llvm::dyn_cast<DerefExprAST>(ast->LHS.get())) {
      if (auto *lhs_var = llvm::dyn_cast<VariableExprAST>(deref->operand.get())) {
        auto *child = find_child(lhs_var->variableName, "*");
        if (child) {
          if (child->state == VarState::Unconsumed && ast->RHS->get_type().linearity == Linearity::Linear) {
            this->add_error(
                ast->get_location(),
                fmt::format("Reassigning linear variable '{}' without "
                            "consuming its previous value",
                            child->name));
            return;
          }
          child->state = VarState::Unconsumed;
          child->def_location = ast->get_location();
        }
      }
    }

    // LHS: check for overwrite without consuming, or re-register
    if (auto *lhs_var = llvm::dyn_cast<VariableExprAST>(ast->LHS.get())) {
      auto *lhs_info = find_linear(lhs_var->variableName);
      if (lhs_info && ast->RHS->get_type().linearity == Linearity::Linear) {
        if (lhs_info->state == VarState::Unconsumed) {
          this->add_error(
              ast->get_location(),
              fmt::format("Reassigning linear variable '{}' without "
                          "consuming its previous value",
                          lhs_var->variableName));
          return;
        }
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
  auto *cp = props_ ? props_->call(ast->id()) : nullptr;
  if (cp && cp->callee_func_type) {
    auto &func_type = std::get<FunctionType>(cp->callee_func_type->type_data);
    auto params = func_type.get_params_types();
    for (size_t i = 0; i < ast->arguments.size() && i < params.size(); i++) {
      if (auto *var =
              llvm::dyn_cast<VariableExprAST>(ast->arguments[i].get())) {
        auto *info = find_linear(var->variableName);
        if (info && params[i].containsLinearTypes()) {
          consume(info, ast->get_location(), "passed to function");
          continue;
        }
      }
      // Handle field access arguments (e.g., some_func(b.data))
      if (auto *fa =
              llvm::dyn_cast<FieldAccessExprAST>(ast->arguments[i].get())) {
        if (auto *obj =
                llvm::dyn_cast<VariableExprAST>(fa->object_expr.get())) {
          auto *child = find_child(obj->variableName, fa->field_name);
          if (child && params[i].containsLinearTypes()) {
            consume(child, ast->get_location(), "passed to function");
            continue;
          }
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

// ── check_addr_of ───────────────────────────────────────────────────

void LinearTypeChecker::check_addr_of(AddrOfExprAST *ast) {
  // Forbid &expr when the operand is linear. Taking the address of a linear
  // value creates a non-linear pointer to it, which would allow aliasing
  // and break the single-owner invariant.
  if (ast->operand->get_type().linearity == Linearity::Linear) {
    this->add_error(
        ast->get_location(),
        fmt::format("Cannot take address of a linear value — "
                    "linear values cannot be aliased"));
    return;
  }
  check_stmt(ast->operand.get());
}

// ── check_deref ─────────────────────────────────────────────────────

void LinearTypeChecker::check_deref(DerefExprAST *ast) {
  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->operand.get())) {
    if (check_use_after_consume(find_linear(var->variableName),
                                ast->get_location(), "dereference"))
      return;
  }
  if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(ast->operand.get())) {
    if (auto *obj = llvm::dyn_cast<VariableExprAST>(fa->object_expr.get())) {
      if (check_use_after_consume(find_child(obj->variableName, fa->field_name),
                                  ast->get_location(), "dereference"))
        return;
    }
  }
  check_stmt(ast->operand.get());
}

// ── check_index ─────────────────────────────────────────────────────

void LinearTypeChecker::check_index(IndexExprAST *ast) {
  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->array_expr.get())) {
    if (check_use_after_consume(find_linear(var->variableName),
                                ast->get_location(), "index"))
      return;
  }
  if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(ast->array_expr.get())) {
    if (auto *obj = llvm::dyn_cast<VariableExprAST>(fa->object_expr.get())) {
      if (check_use_after_consume(find_child(obj->variableName, fa->field_name),
                                  ast->get_location(), "index"))
        return;
    }
  }
  check_stmt(ast->array_expr.get());
  check_stmt(ast->index_expr.get());
}

// ── check_free ──────────────────────────────────────────────────────

void LinearTypeChecker::check_free(FreeExprAST *ast) {
  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->operand.get())) {
    auto *info = find_linear(var->variableName);
    if (info) {
      check_children_consumed(*info);
      consume(info, ast->get_location(), "freed");
      return;
    }
  }
  // Handle free(*pp) — deref operand: consume the "*" child
  if (auto *deref = llvm::dyn_cast<DerefExprAST>(ast->operand.get())) {
    if (auto *var = llvm::dyn_cast<VariableExprAST>(deref->operand.get())) {
      auto *child = find_child(var->variableName, "*");
      if (child) {
        check_children_consumed(*child);
        consume(child, ast->get_location(), "freed");
        return;
      }
    }
  }
  // Handle free(b.data) — field access operand
  if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(ast->operand.get())) {
    if (auto *obj = llvm::dyn_cast<VariableExprAST>(fa->object_expr.get())) {
      auto *child = find_child(obj->variableName, fa->field_name);
      if (child) {
        check_children_consumed(*child);
        consume(child, ast->get_location(), "freed");
        return;
      }
    }
  }
  // Walk operand for nested linear ops
  check_stmt(ast->operand.get());
}

// ── Closure capture tracking ─────────────────────────────────────────

void LinearTypeChecker::record_closure_captures(CallExprAST *ast,
                                                 const std::string &dest_var) {
  auto *cp = props_ ? props_->call(ast->id()) : nullptr;
  if (!cp || !cp->is_partial || !cp->callee_func_type)
    return;

  auto &func_type = std::get<FunctionType>(cp->callee_func_type->type_data);
  auto params = func_type.get_params_types();

  // The bound args are the first N arguments (N < total params → partial app).
  std::vector<Type> captured;
  for (size_t i = 0; i < ast->arguments.size() && i < params.size(); i++)
    captured.push_back(params[i]);

  if (!captured.empty())
    closure_captures_[dest_var] = std::move(captured);
}

// ── check_return ────────────────────────────────────────────────────

void LinearTypeChecker::check_return(ReturnExprAST *ast) {
  if (!ast->return_expr)
    return;

  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->return_expr.get())) {
    auto *info = find_linear(var->variableName);
    if (info) {
      // Returning a linear pointer (or wrapper): ownership transfers to caller
      consume(info, ast->get_location(), "returned");
      return;
    }
    // Returning a type that contains a non-linear pointer — error (may dangle)
    auto path = find_nonlinear_pointer_path(var->get_type());
    if (path) {
      this->add_error(
          ast->get_location(),
          fmt::format("Cannot return '{}' of type '{}': {} "
                      "(may reference a local variable)",
                      var->variableName, var->get_type().to_string(), *path));
      return;
    }
    // Returning a closure — check if any captured arg contains a pointer
    auto it = closure_captures_.find(var->variableName);
    if (it != closure_captures_.end()) {
      for (auto &captured : it->second) {
        if (captured.containsNonLinearPtr()) {
          this->add_error(
              ast->get_location(),
              fmt::format(
                  "Cannot return '{}' — it is a closure that captured a "
                  "non-linear pointer (may dangle after return)",
                  var->variableName));
          return;
        }
      }
    }
  }

  // Handle return b.data — field access returning a linear field
  if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(ast->return_expr.get())) {
    if (auto *obj = llvm::dyn_cast<VariableExprAST>(fa->object_expr.get())) {
      auto *child = find_child(obj->variableName, fa->field_name);
      if (child) {
        consume(child, ast->get_location(), "returned");
        return;
      }
    }
  }

  // Returning a compound literal (struct/tuple/array) that wraps a tainted closure
  auto check_elements_for_tainted_closure =
      [&](const std::vector<std::unique_ptr<ExprAST>> &elements) -> bool {
    for (auto &elem : elements) {
      if (auto *v = llvm::dyn_cast<VariableExprAST>(elem.get())) {
        auto it = closure_captures_.find(v->variableName);
        if (it != closure_captures_.end()) {
          for (auto &captured : it->second) {
            if (captured.containsNonLinearPtr()) {
              this->add_error(
                  ast->get_location(),
                  fmt::format("Cannot return a value containing '{}' — it is "
                              "a closure that captured a non-linear pointer "
                              "(may dangle after return)",
                              v->variableName));
              check_stmt(ast->return_expr.get());
              return true;
            }
          }
        }
      }
    }
    return false;
  };
  if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(ast->return_expr.get())) {
    if (check_elements_for_tainted_closure(sl->field_values))
      return;
  } else if (auto *tl =
                 llvm::dyn_cast<TupleLiteralExprAST>(ast->return_expr.get())) {
    if (check_elements_for_tainted_closure(tl->elements))
      return;
  } else if (auto *al =
                 llvm::dyn_cast<ArrayLiteralExprAST>(ast->return_expr.get())) {
    if (check_elements_for_tainted_closure(al->elements))
      return;
  }

  // Returning a partial application directly (e.g. return some_func(&x))
  if (auto *call = llvm::dyn_cast<CallExprAST>(ast->return_expr.get())) {
    auto *cp = props_ ? props_->call(call->id()) : nullptr;
    if (cp && cp->is_partial && cp->callee_func_type) {
      auto &func_type =
          std::get<FunctionType>(cp->callee_func_type->type_data);
      auto params = func_type.get_params_types();
      for (size_t i = 0; i < call->arguments.size() && i < params.size();
           i++) {
        if (params[i].containsNonLinearPtr()) {
          this->add_error(
              ast->get_location(),
              fmt::format("Cannot return partial application of '{}' — it "
                          "captures a non-linear pointer "
                          "(may dangle after return)",
                          call->functionName.mangled()));
          // Still walk args for other linear checks
          check_stmt(ast->return_expr.get());
          return;
        }
      }
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
  consume_elements(ast->field_values, ast->get_location(), "moved into struct");
}

// ── check_array_literal ─────────────────────────────────────────────

void LinearTypeChecker::check_array_literal(ArrayLiteralExprAST *ast) {
  consume_elements(ast->elements, ast->get_location(), "moved into array");
}

// ── check_tuple_literal ──────────────────────────────────────────────

void LinearTypeChecker::check_tuple_literal(TupleLiteralExprAST *ast) {
  consume_elements(ast->elements, ast->get_location(), "moved into tuple");
}

// ── Inner-linear tracking helpers ────────────────────────────────────

void LinearTypeChecker::register_inner_linear(VarInfo &parent, const Type &t,
                                               sammine_util::Location loc) {
  switch (t.type_kind) {
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(t.type_data);
    auto &names = st.get_field_names();
    auto &types = st.get_field_types();
    for (size_t i = 0; i < names.size(); i++) {
      if (types[i].containsLinearTypes()) {
        auto &child =
            parent.children[names[i]] = VarInfo{VarState::Unconsumed, loc, {},
                                                 parent.name + "." + names[i], {}, {}};
        if (types[i].linearity != Linearity::Linear)
          register_inner_linear(child, types[i], loc);
      }
    }
    break;
  }
  case TypeKind::Tuple: {
    auto &tt = std::get<TupleType>(t.type_data);
    for (size_t i = 0; i < tt.size(); i++) {
      if (tt.get_element(i).containsLinearTypes()) {
        auto key = std::to_string(i);
        auto &child =
            parent.children[key] = VarInfo{VarState::Unconsumed, loc, {},
                                            parent.name + "." + key, {}, {}};
        if (tt.get_element(i).linearity != Linearity::Linear)
          register_inner_linear(child, tt.get_element(i), loc);
      }
    }
    break;
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(t.type_data);
    for (auto &variant : et.get_variants()) {
      for (size_t i = 0; i < variant.payload_types.size(); i++) {
        if (variant.payload_types[i].containsLinearTypes()) {
          auto key = variant.name + "." + std::to_string(i);
          auto &child =
              parent.children[key] = VarInfo{VarState::Unconsumed, loc, {},
                                              parent.name + "." + key, {}, {}};
          if (variant.payload_types[i].linearity != Linearity::Linear)
            register_inner_linear(child, variant.payload_types[i], loc);
        }
      }
    }
    break;
  }
  case TypeKind::Array: {
    auto &at = std::get<ArrayType>(t.type_data);
    if (at.get_element().containsLinearTypes()) {
      auto &child =
          parent.children["*"] = VarInfo{VarState::Unconsumed, loc, {},
                                          parent.name + "[*]", {}, {}};
      if (at.get_element().linearity != Linearity::Linear)
        register_inner_linear(child, at.get_element(), loc);
    }
    break;
  }
  case TypeKind::Pointer: {
    auto &pt = std::get<PointerType>(t.type_data);
    auto pointee = pt.get_pointee();
    if (pointee.containsLinearTypes()) {
      auto &child =
          parent.children["*"] = VarInfo{VarState::Consumed, loc, {},
                                          "*" + parent.name, {}, {}};
      if (pointee.linearity != Linearity::Linear)
        register_inner_linear(child, pointee, loc);
    }
    break;
  }
  case TypeKind::I32_t:
  case TypeKind::I64_t:
  case TypeKind::U32_t:
  case TypeKind::U64_t:
  case TypeKind::F64_t:
  case TypeKind::F32_t:
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::Char:
  case TypeKind::String:
  case TypeKind::Function:
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::Integer:
  case TypeKind::Flt:
  case TypeKind::TypeParam:
  case TypeKind::Generic:
    break;
  }
}

void LinearTypeChecker::check_children_consumed(const VarInfo &info) {
  for (auto &[name, child] : info.children) {
    if (child.state == VarState::Unconsumed) {
      // Check if child has its own children (nested wrapper)
      if (!child.children.empty()) {
        check_children_consumed(child);
      } else {
        this->add_error(
            child.def_location,
            fmt::format(
                "Linear variable '{}' must be consumed before scope exit"
                " (use free() to deallocate)",
                child.name));
      }
    }
  }
}

VarInfo *LinearTypeChecker::find_child(const std::string &var_name,
                                        const std::string &field_name) {
  auto *parent = find_linear(var_name);
  if (!parent)
    return nullptr;
  auto it = parent->children.find(field_name);
  if (it != parent->children.end())
    return &it->second;
  return nullptr;
}

void LinearTypeChecker::register_if_linear(const std::string &name,
                                            const Type &type,
                                            sammine_util::Location loc) {
  if (!type.containsLinearTypes())
    return;
  register_linear(name, loc);
  auto *info = find_linear(name);
  register_inner_linear(*info, type, loc);
}

void LinearTypeChecker::consume_elements(
    const std::vector<std::unique_ptr<ExprAST>> &elements,
    sammine_util::Location loc, const std::string &reason) {
  for (auto &elem : elements) {
    if (auto *var = llvm::dyn_cast<VariableExprAST>(elem.get())) {
      auto *info = find_linear(var->variableName);
      if (info) {
        consume(info, loc, reason);
        continue;
      }
    } else if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(elem.get())) {
      if (auto *obj = llvm::dyn_cast<VariableExprAST>(fa->object_expr.get())) {
        auto *child = find_child(obj->variableName, fa->field_name);
        if (child) {
          consume(child, loc, reason);
          continue;
        }
      }
    }
    check_stmt(elem.get());
  }
}

} // namespace sammine_lang::AST
