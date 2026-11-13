#include "typecheck/LinearTypeChecker.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "typecheck/Types.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>

//! \file LinearTypeChecker.cpp
//! \brief The rules on pointers and linear values that don't depend on
//! control flow. That each linear value is consumed exactly once is checked
//! on MIR, by the ownership check.

namespace sammine_lang::AST {

// ── Entry point ─────────────────────────────────────────────────────

void LinearTypeChecker::check(ProgramAST *program, const ASTProperties &props) {
  props_ = &props;
  check_program(program);
}

void LinearTypeChecker::check_program(ProgramAST *ast) {
  for (auto &def : ast->DefinitionVec) {
    if (auto *func = llvm::dyn_cast<FuncDefAST>(def.get())) {
      check_func(func);
    } else if (auto *inst = llvm::dyn_cast<TypeClassInstanceAST>(def.get())) {
      if (!inst->is_declaration && !inst->is_generic())
        for (auto &method : inst->methods)
          check_func(method.get());
    }
  }
}

void LinearTypeChecker::check_func(FuncDefAST *ast) {
  // Generic templates aren't checked, only their monomorphized copies.
  if (ast->Prototype->is_generic())
    return;
  closure_captures_.clear();
  if (!ast->Block)
    return;
  auto *value = ast->body_value();
  if (!value) {
    check_block(ast->Block.get(),
                std::get<FunctionType>(ast->Prototype->get_type().type_data)
                    .get_return_type());
    return;
  }
  // A body that runs off its end returns its last expression.
  auto &statements = ast->Block->Statements;
  for (auto &stmt : llvm::drop_end(statements))
    check_expr(stmt.get(), Use::Read);
  check_returned_value(value, value->get_location());
}

void LinearTypeChecker::check_block(BlockAST *ast, const Type &result_type) {
  for (auto &stmt : ast->Statements)
    check_expr(stmt.get(), stmt == ast->Statements.back() &&
                                   result_type.containsLinearTypes()
                               ? Use::Own
                               : Use::Read);
}

// Places name existing storage and statements produce no value. Everything
// else makes a new value, a temporary.
static bool is_place_or_statement(const ExprAST *expr) {
  return llvm::isa<VariableExprAST, FieldAccessExprAST, DerefExprAST,
                   IndexExprAST, VarDefAST, ReturnStmtAST>(expr);
}

// ── Expressions ─────────────────────────────────────────────────────

// Every expression is visited with what its parent does with it. A linear
// temporary has no name to be consumed by later, so it may only be where
// it is taken. Writing to a place or taking its address reaches it through
// any field or array element.
void LinearTypeChecker::check_expr(ExprAST *expr, Use use) {
  if (use != Use::Own && !is_place_or_statement(expr) &&
      expr->get_type().containsLinearTypes())
    this->add_error(expr->get_location(),
                    fmt::format("Temporary of linear type '{}' must not be "
                                "discarded (bind it with 'let' first)",
                                expr->get_type().to_string()));

  auto loc = expr->get_location();
  auto read = [&](ExprAST *child) {
    if (child)
      check_expr(child);
  };
  auto own = [&](ExprAST *child) {
    if (child)
      check_expr(child, Use::Own);
  };
  auto own_all = [&](const std::vector<std::unique_ptr<ExprAST>> &children) {
    for (auto &child : children)
      own(child.get());
  };
  // Through a part of a place, writing or taking an address reaches the
  // place; anything else reads it.
  auto toward_place =
      use == Use::Write || use == Use::TakeAddress ? use : Use::Read;

  using NK = NodeKind;
  // No default: a new node kind must be classified before this compiles.
  switch (expr->getKind()) {
  case NK::VariableExprAST:
    break;
  case NK::FieldAccessExprAST:
    check_expr(llvm::cast<FieldAccessExprAST>(expr)->object_expr.get(),
               toward_place);
    break;
  case NK::VarDefAST:
    check_var_def(llvm::cast<VarDefAST>(expr));
    break;
  case NK::BinaryExprAST: {
    auto *bin = llvm::cast<BinaryExprAST>(expr);
    if (bin->Op->is_assign()) {
      check_store(bin);
    } else {
      read(bin->LHS.get());
      read(bin->RHS.get());
    }
    break;
  }
  case NK::CallExprAST:
    check_call(llvm::cast<CallExprAST>(expr));
    break;
  case NK::DropExprAST:
    own(llvm::cast<DropExprAST>(expr)->operand.get());
    break;
  case NK::ReturnStmtAST: {
    auto *ret = llvm::cast<ReturnStmtAST>(expr);
    check_returned_value(ret->return_expr.get(), ret->get_location());
    break;
  }
  case NK::IfExprAST: {
    auto *ifExpr = llvm::cast<IfExprAST>(expr);
    read(ifExpr->bool_expr.get());
    check_block(ifExpr->thenBlockAST.get(), expr->get_type());
    if (ifExpr->elseBlockAST)
      check_block(ifExpr->elseBlockAST.get(), expr->get_type());
    break;
  }
  case NK::WhileExprAST: // rejected by the type checker, so never reached
    break;
  case NK::ForExprAST: {
    auto *forExpr = llvm::cast<ForExprAST>(expr);
    read(forExpr->start.get());
    read(forExpr->end.get());
    check_block(forExpr->body.get(), Type::Unit());
    break;
  }
  case NK::CaseExprAST:
    check_case(llvm::cast<CaseExprAST>(expr));
    break;
  case NK::StructLiteralExprAST:
    own_all(llvm::cast<StructLiteralExprAST>(expr)->field_values);
    break;
  case NK::ArrayLiteralExprAST:
    own_all(llvm::cast<ArrayLiteralExprAST>(expr)->elements);
    break;
  case NK::TupleLiteralExprAST:
    own_all(llvm::cast<TupleLiteralExprAST>(expr)->elements);
    break;
  case NK::DerefExprAST:
    read(llvm::cast<DerefExprAST>(expr)->operand.get());
    break;
  case NK::IndexExprAST: {
    auto *index = llvm::cast<IndexExprAST>(expr);
    auto *base = index->array_expr.get();
    auto base_kind = base->get_type().type_kind;
    if (base_kind == TypeKind::Array)
      check_expr(base, toward_place);
    else
      // Through an Indexer instance, which is lent the collection.
      read(base);
    read(index->index_expr.get());
    break;
  }
  case NK::AddrOfExprAST:
    check_expr(llvm::cast<AddrOfExprAST>(expr)->operand.get(),
               Use::TakeAddress);
    break;
  case NK::RangeExprAST: {
    auto *range = llvm::cast<RangeExprAST>(expr);
    read(range->start.get());
    read(range->end.get());
    break;
  }
  case NK::LenExprAST:
    read(llvm::cast<LenExprAST>(expr)->operand.get());
    break;
  case NK::DimExprAST:
    read(llvm::cast<DimExprAST>(expr)->operand.get());
    break;
  case NK::UnaryNegExprAST:
    read(llvm::cast<UnaryNegExprAST>(expr)->operand.get());
    break;
  case NK::CastExprAST: {
    // A linear operand is passed to Convert, which takes it.
    auto *operand = llvm::cast<CastExprAST>(expr)->operand.get();
    check_expr(operand, operand->get_type().containsLinearTypes() ? Use::Own
                                                                  : Use::Read);
    break;
  }
  case NK::NumberExprAST:
  case NK::StringExprAST:
  case NK::BoolExprAST:
  case NK::CharExprAST:
  case NK::UnitExprAST:
  case NK::AllocExprAST:
    break;
  // Not expressions: never reached from an ExprAST.
  case NK::ProgramAST:
  case NK::PrototypeAST:
  case NK::TypedVarAST:
  case NK::BlockAST:
  case NK::FuncDefAST:
  case NK::ExternAST:
  case NK::StructDefAST:
  case NK::EnumDefAST:
  case NK::TypeAliasDefAST:
  case NK::TypeClassDeclAST:
  case NK::TypeClassInstanceAST:
  case NK::KernelDefAST:
  case NK::KernelNumberExprAST:
  case NK::KernelMapExprAST:
  case NK::KernelReduceExprAST:
    break;
  }
}

// ── Bindings ────────────────────────────────────────────────────────

void LinearTypeChecker::check_var_def(VarDefAST *ast) {
  if (!ast->Expression)
    return;
  check_expr(ast->Expression.get(), Use::Own);
  if (ast->is_tuple_destructure)
    return;

  // Propagate closure captures through variable-to-variable assignment
  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->Expression.get())) {
    auto it = closure_captures_.find(var->variableName.mangled());
    if (it != closure_captures_.end())
      closure_captures_[ast->TypedVar->name] = it->second;
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
            auto it = closure_captures_.find(v->variableName.mangled());
            if (it != closure_captures_.end()) {
              closure_captures_[ast->TypedVar->name] = it->second;
              return;
            }
          }
        }
      };
  if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(ast->Expression.get()))
    propagate_from_elements(sl->field_values);
  else if (auto *tl =
               llvm::dyn_cast<TupleLiteralExprAST>(ast->Expression.get()))
    propagate_from_elements(tl->elements);
  else if (auto *al =
               llvm::dyn_cast<ArrayLiteralExprAST>(ast->Expression.get()))
    propagate_from_elements(al->elements);
}

// ── Non-linear pointer path detection ───────────────────────────────

// Returns a human-readable path to the first non-linear pointer found,
// e.g. "non-linear ptr<i32> in field 'data'", or nullopt if none.
// NOTE: if you add a new wrapping TypeKind to forEachInnerType, add it here
// too.
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
  case TypeKind::Hole:
  case TypeKind::TypeParam:
  case TypeKind::Generic:
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

// ── Stores ──────────────────────────────────────────────────────────

void LinearTypeChecker::check_store(BinaryExprAST *ast) {
  check_expr(ast->LHS.get(), Use::Write);
  check_expr(ast->RHS.get(), Use::Own);
}

// ── Calls ───────────────────────────────────────────────────────────

// The source text of a place an argument names, as `b.p` or `*p`; empty for
// any other expression.
static std::string place_name(ExprAST *expr) {
  if (auto *var = llvm::dyn_cast<VariableExprAST>(expr))
    return var->variableName.mangled();
  if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(expr)) {
    auto object = place_name(fa->object_expr.get());
    return object.empty() ? "" : object + "." + fa->field_name;
  }
  if (auto *deref = llvm::dyn_cast<DerefExprAST>(expr)) {
    auto pointer = place_name(deref->operand.get());
    return pointer.empty() ? "" : "*" + pointer;
  }
  return "";
}

void LinearTypeChecker::check_call(CallExprAST *ast) {
  auto *cp = props_ ? props_->call(ast->id()) : nullptr;
  std::vector<Type> params;
  if (cp && cp->callee_func_type) {
    auto callee_params = std::get<FunctionType>(cp->callee_func_type->type_data)
                             .get_params_types();
    params.assign(callee_params.begin(), callee_params.end());
  }
  for (size_t i = 0; i < ast->arguments.size(); i++) {
    auto *arg = ast->arguments[i].get();
    // A closure can be called any number of times, and each call would hand
    // a bound linear value on again.
    bool is_variadic = !params.empty() && i >= params.size();
    bool takes_ownership =
        !is_variadic && (params.empty() || params[i].containsLinearTypes());
    if (cp && cp->is_partial && takes_ownership) {
      auto name = place_name(arg);
      this->add_error(
          arg->get_location(),
          fmt::format("Cannot bind {} of linear type '{}' into a partial "
                      "application of '{}': the closure can be called more "
                      "than once",
                      name.empty() ? "a value" : fmt::format("'{}'", name),
                      arg->get_type().to_string(),
                      ast->written_name.mangled()));
    }
    check_expr(arg, takes_ownership ? Use::Own : Use::Read);
  }
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

// ── Returns ─────────────────────────────────────────────────────────

void LinearTypeChecker::check_returned_value(ExprAST *value,
                                             sammine_util::Location loc) {
  if (!value)
    return;

  if (auto *var = llvm::dyn_cast<VariableExprAST>(value)) {
    // Returning a linear value hands it to the caller.
    if (var->get_type().containsLinearTypes())
      return;
    // Returning a type that contains a non-linear pointer — error (may dangle)
    auto path = find_nonlinear_pointer_path(var->get_type());
    if (path) {
      this->add_error(loc, fmt::format("Cannot return '{}' of type '{}': {} "
                                       "(may reference a local variable)",
                                       var->variableName.mangled(),
                                       var->get_type().to_string(), *path));
      return;
    }
    // Returning a closure — check if any captured arg contains a pointer
    auto it = closure_captures_.find(var->variableName.mangled());
    if (it != closure_captures_.end()) {
      for (auto &captured : it->second) {
        if (captured.containsNonLinearPtr()) {
          this->add_error(
              loc, fmt::format(
                       "Cannot return '{}' — it is a closure that captured a "
                       "non-linear pointer (may dangle after return)",
                       var->variableName.mangled()));
          return;
        }
      }
    }
  }

  // Returning a compound literal (struct/tuple/array) that wraps a tainted
  // closure
  auto check_elements_for_tainted_closure =
      [&](const std::vector<std::unique_ptr<ExprAST>> &elements) -> bool {
    for (auto &elem : elements) {
      if (auto *v = llvm::dyn_cast<VariableExprAST>(elem.get())) {
        auto it = closure_captures_.find(v->variableName.mangled());
        if (it != closure_captures_.end()) {
          for (auto &captured : it->second) {
            if (captured.containsNonLinearPtr()) {
              this->add_error(
                  loc,
                  fmt::format("Cannot return a value containing '{}' — it is "
                              "a closure that captured a non-linear pointer "
                              "(may dangle after return)",
                              v->variableName.mangled()));
              check_expr(value, Use::Own);
              return true;
            }
          }
        }
      }
    }
    return false;
  };
  if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(value)) {
    if (check_elements_for_tainted_closure(sl->field_values))
      return;
  } else if (auto *tl = llvm::dyn_cast<TupleLiteralExprAST>(value)) {
    if (check_elements_for_tainted_closure(tl->elements))
      return;
  } else if (auto *al = llvm::dyn_cast<ArrayLiteralExprAST>(value)) {
    if (check_elements_for_tainted_closure(al->elements))
      return;
  }

  // Returning a partial application directly (e.g. return some_func(&x))
  if (auto *call = llvm::dyn_cast<CallExprAST>(value)) {
    auto *cp = props_ ? props_->call(call->id()) : nullptr;
    if (cp && cp->is_partial && cp->callee_func_type) {
      auto &func_type = std::get<FunctionType>(cp->callee_func_type->type_data);
      auto params = func_type.get_params_types();
      for (size_t i = 0; i < call->arguments.size() && i < params.size(); i++) {
        if (params[i].containsNonLinearPtr()) {
          this->add_error(
              loc, fmt::format("Cannot return partial application of '{}' — it "
                               "captures a non-linear pointer "
                               "(may dangle after return)",
                               call->functionName.mangled()));
          // Still walk args for other linear checks
          check_expr(value, Use::Own);
          return;
        }
      }
    }
  }

  // Walk into return expression
  check_expr(value, Use::Own);
}

// ── Case ────────────────────────────────────────────────────────────

void LinearTypeChecker::check_wildcard_arm(const CaseExprAST &ast,
                                           const CaseArm &wildcard) {
  auto scrutinee_type = ast.scrutinee->get_type();
  if (scrutinee_type.type_kind != TypeKind::Enum)
    return;

  auto &variants = std::get<EnumType>(scrutinee_type.type_data).get_variants();
  std::vector<bool> named_earlier(variants.size(), false);
  for (auto &arm : ast.arms) {
    if (&arm == &wildcard)
      break;
    if (!arm.pattern.is_wildcard && !arm.pattern.is_literal)
      named_earlier[arm.pattern.variant_index] = true;
  }

  for (size_t i = 0; i < variants.size(); i++) {
    if (named_earlier[i])
      continue;
    for (auto &payload : variants[i].payload_types) {
      if (payload.containsLinearTypes()) {
        this->add_error(
            wildcard.pattern.location,
            fmt::format("Wildcard drops linear payload of variant '{}'; match "
                        "it by name and consume it",
                        variants[i].name));
        break;
      }
    }
  }
}

void LinearTypeChecker::check_case(CaseExprAST *ast) {
  // A case takes its scrutinee apart.
  check_expr(ast->scrutinee.get(), Use::Own);
  for (auto &arm : ast->arms) {
    if (arm.pattern.is_wildcard)
      check_wildcard_arm(*ast, arm);
    if (arm.body)
      check_block(arm.body.get(), ast->get_type());
  }
}

} // namespace sammine_lang::AST
