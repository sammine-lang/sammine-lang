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
  if (ast->Expression)
    check_expr(ast->Expression.get(), Use::Own);
}

// ── Stores ──────────────────────────────────────────────────────────

void LinearTypeChecker::check_store(BinaryExprAST *ast) {
  check_expr(ast->LHS.get(), Use::Write);
  check_expr(ast->RHS.get(), Use::Own);
}

// ── Calls ───────────────────────────────────────────────────────────

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
    check_expr(arg, takes_ownership ? Use::Own : Use::Read);
  }
}

// ── Returns ─────────────────────────────────────────────────────────

void LinearTypeChecker::check_returned_value(ExprAST *value,
                                             sammine_util::Location loc) {
  if (value)
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
