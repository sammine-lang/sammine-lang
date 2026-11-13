#include "typecheck/ViewPlacementCheck.h"
#include "fmt/format.h"
#include "typecheck/Types.h"

//! \file ViewPlacementCheck.cpp
//! \brief Each view where none may be, reported where its type is declared
//! or made.

namespace sammine_lang::AST {

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
        std::get<PointerType>(type.type_data).get_pointee(),
        /*ptr_allowed_here=*/!is_owned);
  }
  case TypeKind::Struct:
    for (auto &field : std::get<StructType>(type.type_data).get_field_types())
      if (has_misplaced_pointer(field, /*ptr_allowed_here=*/false))
        return true;
    return false;
  case TypeKind::Tuple: {
    auto &tuple = std::get<TupleType>(type.type_data);
    for (size_t i = 0; i < tuple.size(); i++)
      if (has_misplaced_pointer(tuple.get_element(i),
                                /*ptr_allowed_here=*/false))
        return true;
    return false;
  }
  case TypeKind::Array:
    return has_misplaced_pointer(
        std::get<ArrayType>(type.type_data).get_element(),
        /*ptr_allowed_here=*/false);
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
    return has_misplaced_pointer(fn.get_return_type(),
                                 /*ptr_allowed_here=*/false);
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
    return false;
  }
  return false;
}

void ViewPlacementCheck::check(const Type &type, bool ptr_allowed_here,
                               sammine_util::Location loc,
                               const std::string &where) {
  if (!has_misplaced_pointer(type, ptr_allowed_here))
    return;
  add_error(loc, fmt::format("Non-owning pointer in {} ('{}'): ptr<T> can "
                             "only be the type of a parameter or local "
                             "variable",
                             where, type.to_string()));
}

void ViewPlacementCheck::check_prototype(PrototypeAST *proto) {
  if (proto->is_generic() || proto->get_type().type_kind != TypeKind::Function)
    return;
  auto fn = std::get<FunctionType>(proto->get_type().type_data);
  auto params = fn.get_params_types();
  for (size_t i = 0; i < proto->parameterVectors.size() && i < params.size();
       i++)
    check(params[i], /*ptr_allowed_here=*/true,
          proto->parameterVectors[i]->get_location(), "a parameter type");
  check(fn.get_return_type(), /*ptr_allowed_here=*/false, proto->get_location(),
        "a return type");
}

void ViewPlacementCheck::visit(StructDefAST *ast) {
  if (ast->type_params.empty())
    check(ast->get_type(), /*ptr_allowed_here=*/false, ast->get_location(),
          "a struct field");
}

void ViewPlacementCheck::visit(EnumDefAST *ast) {
  if (ast->type_params.empty())
    check(ast->get_type(), /*ptr_allowed_here=*/false, ast->get_location(),
          "an enum payload");
}

void ViewPlacementCheck::visit(ExternAST *ast) {
  check_prototype(ast->Prototype.get());
}

void ViewPlacementCheck::visit(FuncDefAST *ast) {
  if (ast->Prototype->is_generic())
    return;
  check_prototype(ast->Prototype.get());
  if (ast->Block)
    ast->Block->accept_vis(this);
}

void ViewPlacementCheck::visit(TypeClassInstanceAST *ast) {
  if (ast->is_declaration || ast->is_generic())
    return;
  ASTVisitor::visit(ast);
}

void ViewPlacementCheck::preorder_walk(VarDefAST *ast) {
  if (ast->is_tuple_destructure) {
    for (auto &var : ast->destructure_vars)
      check(var->get_type(), /*ptr_allowed_here=*/true, var->get_location(),
            "a local variable");
    return;
  }
  check(ast->get_type(), /*ptr_allowed_here=*/true, ast->get_location(),
        "a local variable");
}

void ViewPlacementCheck::preorder_walk(StructLiteralExprAST *ast) {
  check(ast->get_type(), /*ptr_allowed_here=*/false, ast->get_location(),
        "a struct value");
}

void ViewPlacementCheck::preorder_walk(ArrayLiteralExprAST *ast) {
  check(ast->get_type(), /*ptr_allowed_here=*/false, ast->get_location(),
        "an array value");
}

void ViewPlacementCheck::preorder_walk(TupleLiteralExprAST *ast) {
  check(ast->get_type(), /*ptr_allowed_here=*/false, ast->get_location(),
        "a tuple value");
}

} // namespace sammine_lang::AST
