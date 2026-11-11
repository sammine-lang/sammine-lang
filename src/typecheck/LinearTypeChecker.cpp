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
  scope_stack.back()[name] =
      VarInfo{VarState::Unconsumed, loc, {}, name, {}, {}};
}

void LinearTypeChecker::consume(VarInfo *info, sammine_util::Location loc,
                                const std::string &reason) {
  if (info->state == VarState::Consumed) {
    this->add_error(
        loc,
        fmt::format(
            "Cannot use linear variable '{}' — it was already {} previously",
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
  this->add_error(use_loc,
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

LinearTypeChecker::BranchAgreement
LinearTypeChecker::check_agreement(const std::vector<LinearVarMap> &branches,
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
    auto result = check_agreement(
        branches, [&](const LinearVarMap &b) -> std::optional<VarState> {
          auto it = b.find(name);
          return it != b.end() ? std::optional(it->second.state) : std::nullopt;
        });
    apply_consistency(result, info, name, loc);

    // Check children (e.g. v.data inside a struct wrapper)
    for (auto &[child_name, child_before] : before_info.children) {
      if (child_before.state != VarState::Unconsumed)
        continue;

      auto child_result = check_agreement(
          branches, [&](const LinearVarMap &b) -> std::optional<VarState> {
            auto it = b.find(name);
            if (it == b.end())
              return std::nullopt;
            auto cit = it->second.children.find(child_name);
            return cit != it->second.children.end()
                       ? std::optional(cit->second.state)
                       : std::nullopt;
          });
      apply_consistency(child_result,
                        info ? find_child(name, child_name) : nullptr,
                        child_before.name, loc);
    }
  }
}

// ── Pointer placement ───────────────────────────────────────────────
//
// A non-owning ptr<T> may only be the whole type of a parameter or local
// variable. It can never be returned or stored in another type, so it can
// never outlive what it points at.

// True if `type` holds a non-owning pointer where one is not allowed.
// Function types are checked like prototypes: their parameters may be ptr<T>.
static bool has_misplaced_pointer(const Type &type, bool ptr_allowed_here) {
  switch (type.type_kind) {
  case TypeKind::Pointer: {
    bool is_owned = type.linearity == Linearity::Linear;
    if (!is_owned && !ptr_allowed_here)
      return true;
    // A view of a view is fine: nothing can be written through a ptr<T>, so
    // reading the inner pointer only yields another local. Owned memory must
    // not hold a view, since it can outlive what the view points at.
    return has_misplaced_pointer(
        std::get<PointerType>(type.type_data).get_pointee(), /*ptr_allowed_here=*/!is_owned);
  }
  case TypeKind::Struct:
    for (auto &field : std::get<StructType>(type.type_data).get_field_types())
      if (has_misplaced_pointer(field, /*ptr_allowed_here=*/false))
        return true;
    return false;
  case TypeKind::Tuple: {
    auto &tuple = std::get<TupleType>(type.type_data);
    for (size_t i = 0; i < tuple.size(); i++)
      if (has_misplaced_pointer(tuple.get_element(i), /*ptr_allowed_here=*/false))
        return true;
    return false;
  }
  case TypeKind::Array:
    return has_misplaced_pointer(
        std::get<ArrayType>(type.type_data).get_element(), /*ptr_allowed_here=*/false);
  case TypeKind::Enum:
    for (auto &variant : std::get<EnumType>(type.type_data).get_variants())
      for (auto &payload : variant.payload_types)
        if (has_misplaced_pointer(payload, /*ptr_allowed_here=*/false))
          return true;
    return false;
  case TypeKind::Function: {
    auto &fn = std::get<FunctionType>(type.type_data);
    for (auto &param : fn.get_params_types())
      if (has_misplaced_pointer(param, /*ptr_allowed_here=*/true))
        return true;
    return has_misplaced_pointer(fn.get_return_type(), /*ptr_allowed_here=*/false);
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
    return false;
  }
  return false;
}

void LinearTypeChecker::check_placement(const Type &type,
                                        bool ptr_allowed_here,
                                        sammine_util::Location loc,
                                        const std::string &where) {
  if (!has_misplaced_pointer(type, ptr_allowed_here))
    return;
  this->add_error(loc, fmt::format("Non-owning pointer in {} ('{}'): ptr<T> "
                                   "can only be the type of a parameter or "
                                   "local variable",
                                   where, type.to_string()));
}

void LinearTypeChecker::check_prototype_placement(PrototypeAST *proto) {
  if (proto->is_generic() || proto->get_type().type_kind != TypeKind::Function)
    return;
  auto fn = std::get<FunctionType>(proto->get_type().type_data);
  auto params = fn.get_params_types();
  for (size_t i = 0; i < proto->parameterVectors.size() && i < params.size();
       i++)
    check_placement(params[i], /*ptr_allowed_here=*/true, proto->parameterVectors[i]->get_location(),
                    "a parameter type");
  check_placement(fn.get_return_type(), /*ptr_allowed_here=*/false, proto->get_location(),
                  "a return type");
}

// ── Entry point ─────────────────────────────────────────────────────

void LinearTypeChecker::check(ProgramAST *program, const ASTProperties &props) {
  props_ = &props;
  check_program(program);
}

void LinearTypeChecker::check_program(ProgramAST *ast) {
  for (auto &def : ast->DefinitionVec) {
    if (auto *func = llvm::dyn_cast<FuncDefAST>(def.get())) {
      check_func(func);
    } else if (auto *ext = llvm::dyn_cast<ExternAST>(def.get())) {
      check_prototype_placement(ext->Prototype.get());
    } else if (auto *sd = llvm::dyn_cast<StructDefAST>(def.get())) {
      if (sd->type_params.empty())
        check_placement(sd->get_type(), /*ptr_allowed_here=*/false, sd->get_location(),
                        "a struct field");
    } else if (auto *ed = llvm::dyn_cast<EnumDefAST>(def.get())) {
      if (ed->type_params.empty())
        check_placement(ed->get_type(), /*ptr_allowed_here=*/false, ed->get_location(),
                        "an enum payload");
    } else if (auto *inst = llvm::dyn_cast<TypeClassInstanceAST>(def.get())) {
      if (!inst->is_declaration)
        for (auto &method : inst->methods)
          check_func(method.get());
    }
  }
}

void LinearTypeChecker::check_func(FuncDefAST *ast) {
  // Skip generic function templates — only check monomorphized copies.
  if (ast->Prototype->is_generic())
    return;

  check_prototype_placement(ast->Prototype.get());
  push_scope();

  // Register linear parameters (including wrapper types with linear fields)
  for (auto &param : ast->Prototype->parameterVectors)
    register_if_linear(param->name, param->get_type(), param->get_location());

  if (ast->Block) {
    auto fn_type = std::get<FunctionType>(ast->Prototype->get_type().type_data);
    check_block(ast->Block.get(), fn_type.get_return_type());
  }

  pop_scope_and_check(ast->get_location());
}

// The last statement is the block's value: it is only kept if whatever owns
// the block produces a linear result.
void LinearTypeChecker::check_block(BlockAST *ast, const Type &result_type) {
  for (auto &stmt : ast->Statements) {
    bool is_kept_value =
        stmt == ast->Statements.back() && result_type.containsLinearTypes();
    auto loc = stmt->get_location();
    check_expr(stmt.get(),
               is_kept_value ? Use::owns(loc, "moved") : Use::reads(loc, "use"));
  }
}

// ── Recursive dispatch ──────────────────────────────────────────────

// Places name existing storage and statements produce no value. Everything
// else creates a new value, so a new expression kind is a temporary by default.
static bool is_place_or_statement(const ExprAST *expr) {
  return llvm::isa<VariableExprAST, FieldAccessExprAST, DerefExprAST,
                   IndexExprAST, VarDefAST, ReturnStmtAST>(expr);
}

// Every expression is visited with what its parent does with it. A linear
// temporary has no name to track, so it is only legal where ownership is taken.
void LinearTypeChecker::check_expr(ExprAST *expr, Use use) {
  if (use.kind != Use::Kind::Own && !is_place_or_statement(expr) &&
      expr->get_type().containsLinearTypes()) {
    this->add_error(
        expr->get_location(),
        fmt::format("Temporary of linear type '{}' must not be discarded "
                    "(bind it with 'let' first)",
                    expr->get_type().to_string()));
  }

  auto loc = expr->get_location();
  auto read_child = [&](ExprAST *child) {
    check_expr(child, Use::reads(loc, "use"));
  };

  using NK = NodeKind;
  // No default: a new node kind must be classified before this compiles.
  switch (expr->getKind()) {
  case NK::VariableExprAST:
    check_variable(llvm::cast<VariableExprAST>(expr), use);
    break;
  case NK::FieldAccessExprAST:
    check_field_access(llvm::cast<FieldAccessExprAST>(expr), use);
    break;
  case NK::VarDefAST:
    check_var_def(llvm::cast<VarDefAST>(expr));
    break;
  case NK::BinaryExprAST:
    check_binary(llvm::cast<BinaryExprAST>(expr));
    break;
  case NK::CallExprAST:
    check_call(llvm::cast<CallExprAST>(expr));
    break;
  case NK::FreeExprAST:
    check_free(llvm::cast<FreeExprAST>(expr));
    break;
  case NK::ReturnStmtAST:
    check_return(llvm::cast<ReturnStmtAST>(expr));
    break;
  case NK::IfExprAST:
    check_if(llvm::cast<IfExprAST>(expr));
    break;
  case NK::WhileExprAST: // rejected by the type checker, so never reached
    break;
  case NK::ForExprAST:
    check_for(llvm::cast<ForExprAST>(expr));
    break;
  case NK::CaseExprAST:
    check_case(llvm::cast<CaseExprAST>(expr));
    break;
  case NK::StructLiteralExprAST:
    check_placement(expr->get_type(), /*ptr_allowed_here=*/false, loc, "a struct value");
    move_elements(llvm::cast<StructLiteralExprAST>(expr)->field_values, loc,
                  "moved into struct");
    break;
  case NK::ArrayLiteralExprAST:
    check_placement(expr->get_type(), /*ptr_allowed_here=*/false, loc, "an array value");
    move_elements(llvm::cast<ArrayLiteralExprAST>(expr)->elements, loc,
                  "moved into array");
    break;
  case NK::TupleLiteralExprAST:
    check_placement(expr->get_type(), /*ptr_allowed_here=*/false, loc, "a tuple value");
    move_elements(llvm::cast<TupleLiteralExprAST>(expr)->elements, loc,
                  "moved into tuple");
    break;
  case NK::DerefExprAST:
    check_through_pointer(llvm::cast<DerefExprAST>(expr)->operand.get(), use,
                          loc, "dereference");
    break;
  case NK::IndexExprAST: {
    auto *index = llvm::cast<IndexExprAST>(expr);
    auto *base = index->array_expr.get();
    if (base->get_type().type_kind == TypeKind::Pointer)
      check_through_pointer(base, use, loc, "index");
    else
      check_expr(base, use.toward_place(loc, "index"));
    read_child(index->index_expr.get());
    break;
  }
  case NK::AddrOfExprAST:
    check_addr_of(llvm::cast<AddrOfExprAST>(expr));
    break;
  case NK::RangeExprAST: {
    auto *range = llvm::cast<RangeExprAST>(expr);
    read_child(range->start.get());
    read_child(range->end.get());
    break;
  }
  case NK::LenExprAST:
    read_child(llvm::cast<LenExprAST>(expr)->operand.get());
    break;
  case NK::DimExprAST:
    read_child(llvm::cast<DimExprAST>(expr)->operand.get());
    break;
  case NK::UnaryNegExprAST:
    read_child(llvm::cast<UnaryNegExprAST>(expr)->operand.get());
    break;
  case NK::CastExprAST: {
    // The operand is passed to Convert, which owns what it is given.
    auto *operand = llvm::cast<CastExprAST>(expr)->operand.get();
    if (operand->get_type().containsLinearTypes())
      check_expr(operand, Use::owns(loc, "converted"));
    else
      read_child(operand);
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

void LinearTypeChecker::apply_use(VarInfo *info, Use use) {
  switch (use.kind) {
  case Use::Kind::Own:
    consume(info, use.loc, use.action);
    break;
  case Use::Kind::Read:
  case Use::Kind::TakeAddress:
    check_use_after_consume(info, use.loc, use.action);
    break;
  case Use::Kind::Write:
    break; // Overwriting a consumed variable or field is valid.
  }
}

// Reaching memory through a pointer: only an owner may write, and a plain
// pointer into owned memory would still be readable after free.
void LinearTypeChecker::check_through_pointer(ExprAST *pointer, Use use,
                                              sammine_util::Location loc,
                                              const char *verb) {
  bool is_owned = pointer->get_type().linearity == Linearity::Linear;
  if (use.kind == Use::Kind::Write && !is_owned) {
    this->add_error(loc, fmt::format("Cannot write through non-owning pointer "
                                     "'{}' — only an owned pointer can be "
                                     "written",
                                     pointer->get_type().to_string()));
    return;
  }
  if (use.kind == Use::Kind::TakeAddress && is_owned) {
    this->add_error(loc, "Cannot take the address of memory owned by a linear "
                         "pointer — it could dangle after free");
    return;
  }
  check_expr(pointer, Use::reads(loc, verb));
}

void LinearTypeChecker::check_variable(VariableExprAST *ast, Use use) {
  if (auto *info = find_linear(ast->variableName.mangled()))
    apply_use(info, use);
}

VarInfo *LinearTypeChecker::find_field(FieldAccessExprAST *ast) {
  auto *obj = llvm::dyn_cast<VariableExprAST>(ast->object_expr.get());
  return obj ? find_child(obj->variableName.mangled(), ast->field_name)
             : nullptr;
}

void LinearTypeChecker::check_field_access(FieldAccessExprAST *ast, Use use) {
  if (auto *child = find_field(ast)) {
    apply_use(child, use);
    return;
  }
  // Non-linear field: moving the field only reads the object.
  check_expr(ast->object_expr.get(), use.toward_place(use.loc, use.action));
}

void LinearTypeChecker::move_elements(
    const std::vector<std::unique_ptr<ExprAST>> &elements,
    sammine_util::Location loc, const char *reason) {
  for (auto &elem : elements)
    check_expr(elem.get(), Use::owns(loc, reason));
}

// ── check_var_def ───────────────────────────────────────────────────

void LinearTypeChecker::check_var_def(VarDefAST *ast) {
  if (ast->is_tuple_destructure) {
    for (auto &var : ast->destructure_vars)
      check_placement(var->get_type(), /*ptr_allowed_here=*/true, var->get_location(),
                      "a local variable");
  } else {
    check_placement(ast->get_type(), /*ptr_allowed_here=*/true, ast->get_location(),
                    "a local variable");
  }
  if (!ast->Expression)
    return;

  auto loc = ast->get_location();

  // Tuple destructuring: let (a, b) = expr;
  if (ast->is_tuple_destructure) {
    check_expr(ast->Expression.get(), Use::owns(loc, "destructured"));
    for (auto &var : ast->destructure_vars)
      register_if_linear(var->name, var->get_type(), var->get_location());
    return;
  }

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

  check_expr(ast->Expression.get(), Use::owns(loc, "moved"));

  register_if_linear(ast->TypedVar->name, ast->get_type(), loc);
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
  case TypeKind::TypeParam:
  case TypeKind::Generic:
    return std::nullopt;
  default:
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

    check_expr(ast->LHS.get(), Use::writes(ast->get_location()));

    check_expr(ast->RHS.get(), Use::owns(ast->get_location(), "moved"));

    // LHS deref: *pp = x updates the "*" child's location
    if (auto *deref = llvm::dyn_cast<DerefExprAST>(ast->LHS.get())) {
      if (auto *lhs_var =
              llvm::dyn_cast<VariableExprAST>(deref->operand.get())) {
        auto *child = find_child(lhs_var->variableName.mangled(), "*");
        if (child) {
          if (child->state == VarState::Unconsumed &&
              ast->RHS->get_type().linearity == Linearity::Linear) {
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
      auto *lhs_info = find_linear(lhs_var->variableName.mangled());
      if (lhs_info && ast->RHS->get_type().linearity == Linearity::Linear) {
        if (lhs_info->state == VarState::Unconsumed) {
          this->add_error(
              ast->get_location(),
              fmt::format("Reassigning linear variable '{}' without "
                          "consuming its previous value",
                          lhs_var->variableName.mangled()));
          return;
        }
        lhs_info->state = VarState::Unconsumed;
        lhs_info->def_location = ast->get_location();
      }
    }
  } else {
    auto read = Use::reads(ast->get_location(), "use");
    check_expr(ast->LHS.get(), read);
    check_expr(ast->RHS.get(), read);
  }
}

// ── check_call ──────────────────────────────────────────────────────

void LinearTypeChecker::check_call(CallExprAST *ast) {
  auto *cp = props_ ? props_->call(ast->id()) : nullptr;
  std::vector<Type> params;
  if (cp && cp->callee_func_type) {
    auto callee_params = std::get<FunctionType>(cp->callee_func_type->type_data)
                             .get_params_types();
    params.assign(callee_params.begin(), callee_params.end());
  }

  auto loc = ast->get_location();
  std::vector<VarInfo *> lent, moved;
  for (size_t i = 0; i < ast->arguments.size(); i++) {
    auto *arg = ast->arguments[i].get();
    // Variadic arguments have no parameter to take ownership. With no callee
    // information, the type checker has already matched any linear argument.
    // A linear argument to a non-linear parameter is lent, not moved.
    bool is_variadic = !params.empty() && i >= params.size();
    bool takes_ownership =
        !is_variadic && (params.empty() || params[i].containsLinearTypes());
    if (auto *info = find_tracked(arg))
      (takes_ownership ? moved : lent).push_back(info);
    check_expr(arg, takes_ownership ? Use::owns(loc, "passed to function")
                                    : Use::reads(loc, "use"));
  }

  // The callee could free what it was given while still reading the lent view.
  for (auto *view : lent) {
    for (auto *owner : moved) {
      if (view == owner || contains_tracked(*owner, view)) {
        this->add_error(loc, fmt::format("Cannot lend '{}' and move it in the "
                                         "same call",
                                         view->name));
      }
    }
  }
}

VarInfo *LinearTypeChecker::find_tracked(ExprAST *expr) {
  if (auto *var = llvm::dyn_cast<VariableExprAST>(expr))
    return find_linear(var->variableName.mangled());
  if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(expr))
    return find_field(fa);
  return nullptr;
}

bool LinearTypeChecker::contains_tracked(const VarInfo &owner,
                                         const VarInfo *part) {
  for (auto &[name, child] : owner.children)
    if (&child == part || contains_tracked(child, part))
      return true;
  return false;
}

// ── check_addr_of ───────────────────────────────────────────────────

void LinearTypeChecker::check_addr_of(AddrOfExprAST *ast) {
  // Forbid &expr when the operand is linear. Taking the address of a linear
  // value creates a non-linear pointer to it, which would allow aliasing
  // and break the single-owner invariant.
  if (ast->operand->get_type().linearity == Linearity::Linear) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot take address of a linear value — "
                                "linear values cannot be aliased"));
    return;
  }
  check_expr(ast->operand.get(), Use::takes_address(ast->get_location()));
}

// ── check_free ──────────────────────────────────────────────────────

void LinearTypeChecker::check_free(FreeExprAST *ast) {
  auto *operand = ast->operand.get();
  VarInfo *target = nullptr;
  if (auto *var = llvm::dyn_cast<VariableExprAST>(operand)) {
    target = find_linear(var->variableName.mangled());
  } else if (auto *deref = llvm::dyn_cast<DerefExprAST>(operand)) {
    if (auto *base = llvm::dyn_cast<VariableExprAST>(deref->operand.get()))
      target = find_child(base->variableName.mangled(), "*");
  } else if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(operand)) {
    target = find_field(fa);
  }

  if (target) {
    check_children_consumed(*target);
    consume(target, ast->get_location(), "freed");
    return;
  }
  check_expr(operand, Use::owns(ast->get_location(), "freed"));
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

void LinearTypeChecker::check_return(ReturnStmtAST *ast) {
  if (!ast->return_expr)
    return;

  if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->return_expr.get())) {
    if (find_linear(var->variableName.mangled())) {
      // Returning a linear pointer (or wrapper): ownership transfers to caller
      check_expr(var, Use::owns(ast->get_location(), "returned"));
      return;
    }
    // Returning a type that contains a non-linear pointer — error (may dangle)
    auto path = find_nonlinear_pointer_path(var->get_type());
    if (path) {
      this->add_error(ast->get_location(),
                      fmt::format("Cannot return '{}' of type '{}': {} "
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
              ast->get_location(),
              fmt::format(
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
                  ast->get_location(),
                  fmt::format("Cannot return a value containing '{}' — it is "
                              "a closure that captured a non-linear pointer "
                              "(may dangle after return)",
                              v->variableName.mangled()));
              check_expr(ast->return_expr.get(), Use::owns(ast->get_location(), "returned"));
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
      auto &func_type = std::get<FunctionType>(cp->callee_func_type->type_data);
      auto params = func_type.get_params_types();
      for (size_t i = 0; i < call->arguments.size() && i < params.size(); i++) {
        if (params[i].containsNonLinearPtr()) {
          this->add_error(
              ast->get_location(),
              fmt::format("Cannot return partial application of '{}' — it "
                          "captures a non-linear pointer "
                          "(may dangle after return)",
                          call->functionName.mangled()));
          // Still walk args for other linear checks
          check_expr(ast->return_expr.get(), Use::owns(ast->get_location(), "returned"));
          return;
        }
      }
    }
  }

  // Walk into return expression
  check_expr(ast->return_expr.get(), Use::owns(ast->get_location(), "returned"));
}

// ── check_if ────────────────────────────────────────────────────────

void LinearTypeChecker::check_if(IfExprAST *ast) {
  // Condition can consume (unconditional)
  check_expr(ast->bool_expr.get(), Use::reads(ast->get_location(), "use"));

  auto before = snapshot();

  // Then branch
  check_block(ast->thenBlockAST.get(), ast->get_type());
  auto state_then = snapshot();

  // Restore for else
  restore(before);

  LinearVarMap state_else;
  if (ast->elseBlockAST) {
    check_block(ast->elseBlockAST.get(), ast->get_type());
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

// ── check_loop_body ─────────────────────────────────────────────────

void LinearTypeChecker::check_loop_body(std::function<void()> check_body) {
  auto before = snapshot();
  loop_depth++;

  check_body();

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
          fmt::format("Cannot consume linear variable '{}' inside a loop"
                      " (defined outside the loop)",
                      name));
    }
  }

  // Restore state (loop might run 0 times, so outer state is unchanged)
  restore(before);
}

// ── check_for ───────────────────────────────────────────────────────

void LinearTypeChecker::check_for(ForExprAST *ast) {
  check_loop_body([&] {
    auto read = Use::reads(ast->get_location(), "use");
    check_expr(ast->start.get(), read);
    check_expr(ast->end.get(), read);
    check_block(ast->body.get(), Type::Unit());
  });
}

// ── check_case ──────────────────────────────────────────────────────

void LinearTypeChecker::register_case_bindings(const CasePattern &pattern,
                                               const Type &scrutinee_type) {
  if (pattern.is_wildcard || pattern.is_literal ||
      scrutinee_type.type_kind != TypeKind::Enum)
    return;

  auto &enum_type = std::get<EnumType>(scrutinee_type.type_data);
  auto &payloads = enum_type.get_variants()[pattern.variant_index].payload_types;
  for (size_t i = 0; i < pattern.bindings.size() && i < payloads.size(); i++)
    register_if_linear(pattern.bindings[i], payloads[i], pattern.location);
}

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
  // A linear scrutinee is taken apart: ownership moves to the arm's bindings.
  check_expr(ast->scrutinee.get(),
             Use::owns(ast->get_location(), "matched in case"));

  auto before = snapshot();
  std::vector<LinearVarMap> arm_states;

  for (auto &arm : ast->arms) {
    restore(before);
    if (arm.pattern.is_wildcard)
      check_wildcard_arm(*ast, arm);
    push_scope();
    register_case_bindings(arm.pattern, ast->scrutinee->get_type());
    if (arm.body)
      check_block(arm.body.get(), ast->get_type());
    pop_scope_and_check(arm.pattern.location);
    arm_states.push_back(snapshot());
  }

  restore(before);
  check_branch_consistency(before, arm_states, ast->get_location());
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
        auto &child = parent.children[names[i]] =
            VarInfo{VarState::Unconsumed,         loc, {},
                    parent.name + "." + names[i], {},  {}};
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
        auto &child = parent.children[key] = VarInfo{
            VarState::Unconsumed, loc, {}, parent.name + "." + key, {}, {}};
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
          auto &child = parent.children[key] = VarInfo{
              VarState::Unconsumed, loc, {}, parent.name + "." + key, {}, {}};
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
      auto &child = parent.children["*"] =
          VarInfo{VarState::Unconsumed, loc, {}, parent.name + "[*]", {}, {}};
      if (at.get_element().linearity != Linearity::Linear)
        register_inner_linear(child, at.get_element(), loc);
    }
    break;
  }
  case TypeKind::Pointer: {
    auto &pt = std::get<PointerType>(t.type_data);
    auto pointee = pt.get_pointee();
    if (pointee.containsLinearTypes()) {
      auto &child = parent.children["*"] =
          VarInfo{VarState::Consumed, loc, {}, "*" + parent.name, {}, {}};
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

} // namespace sammine_lang::AST
