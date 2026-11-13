#include "semantics/GeneralSemanticsVisitor.h"
#include "ast/Ast.h"
#include "util/Utilities.h"
#include <fmt/core.h>
//! \file GeneralSemanticsVisitor.cpp
//! \brief Implementation for GeneralSemanticsVisitor
namespace sammine_lang::AST {

void GeneralSemanticsVisitor::check_reserved_identifier(
    const std::string &name, sammine_util::Location loc) {
  if (name.find("__") != std::string::npos) {
    this->add_error(
        loc,
        std::vector<std::string>{
            fmt::format("Identifier '{}' contains '__', which is reserved for "
                        "C interop name mangling",
                        name),
            "Please use single underscore '_'"});
  }
}

void GeneralSemanticsVisitor::preorder_walk(FuncDefAST *ast) {
  // Check function name for reserved "__"
  check_reserved_identifier(ast->Prototype->functionName.get_name(),
                            ast->Prototype->get_location());
}

void GeneralSemanticsVisitor::preorder_walk(VarDefAST *ast) {
  if (ast->is_tuple_destructure) {
    for (auto &var : ast->destructure_vars)
      check_reserved_identifier(var->name, var->get_location());
  } else {
    check_reserved_identifier(ast->TypedVar->name, ast->get_location());
  }
}

void GeneralSemanticsVisitor::preorder_walk(StructDefAST *ast) {
  check_reserved_identifier(ast->struct_name.get_name(), ast->get_location());
}

void GeneralSemanticsVisitor::preorder_walk(EnumDefAST *ast) {
  check_reserved_identifier(ast->enum_name.get_name(), ast->get_location());
}
void GeneralSemanticsVisitor::preorder_walk(TypeAliasDefAST *ast) {
  check_reserved_identifier(ast->alias_name.get_name(), ast->get_location());
}
} // namespace sammine_lang::AST
