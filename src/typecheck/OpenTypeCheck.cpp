#include "typecheck/OpenTypeCheck.h"
#include "typecheck/Types.h"

#include <algorithm>

//! \file OpenTypeCheck.cpp
//! \brief The type arguments nothing decided, each reported once, at the
//! variant it starts from.

namespace sammine_lang::AST {

void OpenTypeCheck::visit(FuncDefAST *ast) {
  if (ast->Prototype->is_generic() || !ast->Block)
    return;
  ASTVisitor::visit(ast);
}

void OpenTypeCheck::visit(TypeClassInstanceAST *ast) {
  if (ast->is_declaration || ast->is_generic())
    return;
  ASTVisitor::visit(ast);
}

void OpenTypeCheck::preorder_walk(VariableExprAST *ast) {
  if (props_.variable(ast->id()) &&
      props_.variable(ast->id())->is_enum_unit_variant)
    check(ast, ast->variableName.get_name());
}

void OpenTypeCheck::preorder_walk(CallExprAST *ast) {
  if (props_.call(ast->id()) && props_.call(ast->id())->is_enum_constructor)
    check(ast, ast->functionName.get_name());
}

void OpenTypeCheck::check(const ExprAST *expr, const std::string &variant) {
  auto type = expr->get_type();
  if (type.type_kind != TypeKind::Enum)
    return;
  auto *instance_of = type.instance_of();
  if (!instance_of ||
      std::ranges::none_of(instance_of->type_args, [](const Type &arg) {
        return arg.type_kind == TypeKind::Hole;
      }))
    return;
  add_error(expr->get_location(), open_type_message(type, variant));
}

} // namespace sammine_lang::AST
