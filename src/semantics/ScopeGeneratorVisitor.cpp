#include "semantics/ScopeGeneratorVisitor.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "util/LexicalContext.h"
#include "util/Utilities.h"
#include <ctime>
#include <memory>
//! \file ScopeGeneratorVisitor.cpp
//! \brief Implements ScopeGeneratorVisitor, an ASTVisitor that traverses the
//! AST to populate a lexical symbol table
namespace sammine_lang::AST {
// pre order
void ScopeGeneratorVisitor::preorder_walk(ProgramAST *ast) {

  std::string fn_name = "";
  sammine_util::Location loc;
  for (auto &def : ast->DefinitionVec) {
    if (auto fn_def = dynamic_cast<FuncDefAST *>(def.get())) {
      fn_name = fn_def->Prototype->functionName;
      loc = fn_def->Prototype->get_location();
    } else if (auto extern_def = dynamic_cast<ExternAST *>(def.get())) {
      fn_name = extern_def->Prototype->functionName;
      loc = extern_def->Prototype->get_location();
    } else if (auto record_def = dynamic_cast<RecordDefAST *>(def.get())) {
      fn_name = record_def->record_name;
      loc = record_def->get_location();
    } else
      this->abort("Should not be any other def");

    if (fn_name != "") {
      if (can_see(fn_name) == nameNotFound)
        register_name(fn_name, loc);
      else if (can_see(fn_name) == nameFound) {
        add_error(loc, fmt::format(
                           "[SCOPE1]: The name `{}` has been introduced before",
                           fn_name));
        add_error(this->scope_stack.get_from_name(fn_name),
                  fmt::format("[SCOPE1]: Most recently defined `{}` is here",
                              fn_name));
      }
    }
  }
}
void ScopeGeneratorVisitor::preorder_walk(VarDefAST *ast) {

  auto var_name = ast->TypedVar->name;
  if (can_see(var_name) == nameNotFound) {
    register_name(var_name, ast->TypedVar->get_location());
  } else if (can_see(var_name) == nameFound) {
    add_error(ast->get_location(),
              fmt::format("[SCOPE1]: The name `{}` has been introduced before",
                          var_name));
    add_error(
        this->scope_stack.recursive_get_from_name(var_name),
        fmt::format("[SCOPE1]: The firstly defined `{}` is here", var_name));
  }
}
void ScopeGeneratorVisitor::preorder_walk(ExternAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(FuncDefAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(RecordDefAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(PrototypeAST *ast) {
  // get previous scope and register the function name
  auto var_name = ast->functionName;
  if (can_see_parent(var_name) == nameNotFound)
    register_name_parent(var_name, ast->get_location());

  for (auto &param : ast->parameterVectors)
    register_name(param->name, param->get_location());
}
void ScopeGeneratorVisitor::preorder_walk(CallExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(ReturnExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(BinaryExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(NumberExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(StringExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(BoolExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(VariableExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(BlockAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(IfExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(UnitExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(TypedVarAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(DerefExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(AddrOfExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(AllocExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(FreeExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(ArrayLiteralExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(IndexExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(LenExprAST *ast) {}

// post order
void ScopeGeneratorVisitor::postorder_walk(ProgramAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(VarDefAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(ExternAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(FuncDefAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(RecordDefAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(PrototypeAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(CallExprAST *ast) {

  auto var_name = ast->functionName;
  if (can_see_parent(var_name) ==
      nameNotFound) {
    add_error(
        ast->get_location(),
        fmt::format(
            "The called name {} for the call expression is not found before",
            var_name));
  }
}

void ScopeGeneratorVisitor::postorder_walk(ReturnExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(BinaryExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(NumberExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(StringExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(BoolExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(VariableExprAST *ast) {

  auto var_name = ast->variableName;
  if (can_see(var_name) == nameNotFound) {
    add_error(ast->get_location(),
              fmt::format("The variable named {} for the variable expression "
                          "is not found before",
                          var_name));
  }
}
void ScopeGeneratorVisitor::postorder_walk(BlockAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(IfExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(UnitExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(TypedVarAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(DerefExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(AddrOfExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(AllocExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(FreeExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(ArrayLiteralExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(IndexExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(LenExprAST *ast) {}
} // namespace sammine_lang::AST
