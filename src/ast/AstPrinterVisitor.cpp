#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "fmt/base.h"
#include "fmt/format.h"
#include <string_view>
//! \file AstPrinterVisitor.cpp
//! \brief Implementation for Ast Printer, using a Visitor pattern in order to
//! traverse the AST for better debugging
namespace sammine_lang::AST {

class AstPrinterVisitor : public ScopedASTVisitor {
  const std::string disclaimer =
      ""
      "---PrinterINFO---\n\n"
      "  \"()\"       : In function parameter, this means empty\n"
      "               In return type, this means unit\n"
      "               Sometimes used as tuple.\n\n"
      "  \"??\"       : Type is NonExistent, not yet initialized.\n"
      "  \"Poisoned\" : There is an error in typechecking.\n"
      "  \"!!\"       : Error in Parsing stage/ Parser letting in a nullptr.\n"
      "\n---PrinterINFO---\n\n"
      "";
  const std::string tab = "  ";
  std::string rep = "";
  std::string current_tabs = "";
  std::string_view tabs() const;
  void generic_preprintln(AstBase *ast);
  void generic_preprint(AstBase *ast);
  void generic_postprint();
  void add_to_rep(const std::string &s);
  void trim_rep();

public:
  virtual void enter_new_scope() override { current_tabs += tab; }
  virtual void exit_new_scope() override {
    current_tabs.pop_back();
    current_tabs.pop_back();
  }

  virtual void visit(ProgramAST *ast) override;

  virtual void visit(VarDefAST *ast) override;

  virtual void visit(ExternAST *ast) override;

  virtual void visit(FuncDefAST *ast) override;

  virtual void visit(StructDefAST *ast) override;

  virtual void visit(PrototypeAST *ast) override;

  virtual void visit(CallExprAST *ast) override;

  virtual void visit(BinaryExprAST *ast) override;

  virtual void visit(NumberExprAST *ast) override;
  virtual void visit(StringExprAST *ast) override;

  virtual void visit(BoolExprAST *ast) override;

  virtual void visit(VariableExprAST *ast) override;

  virtual void visit(BlockAST *ast) override;
  virtual void visit(ReturnExprAST *ast) override;

  virtual void visit(IfExprAST *ast) override;
  virtual void visit(UnitExprAST *ast) override;

  virtual void visit(TypedVarAST *ast) override;
  virtual void visit(DerefExprAST *ast) override;
  virtual void visit(AddrOfExprAST *ast) override;
  virtual void visit(AllocExprAST *ast) override;
  virtual void visit(FreeExprAST *ast) override;
  virtual void visit(ArrayLiteralExprAST *ast) override;
  virtual void visit(IndexExprAST *ast) override;
  virtual void visit(LenExprAST *ast) override;
  virtual void visit(UnaryNegExprAST *ast) override;
  virtual void visit(StructLiteralExprAST *ast) override;
  virtual void visit(FieldAccessExprAST *ast) override;
  // pre order
  virtual void preorder_walk(ProgramAST *ast) override;
  virtual void preorder_walk(VarDefAST *ast) override;
  virtual void preorder_walk(ExternAST *ast) override;
  virtual void preorder_walk(FuncDefAST *ast) override;
  virtual void preorder_walk(StructDefAST *ast) override;
  virtual void preorder_walk(PrototypeAST *ast) override;
  virtual void preorder_walk(CallExprAST *ast) override;
  virtual void preorder_walk(ReturnExprAST *ast) override;
  virtual void preorder_walk(BinaryExprAST *ast) override;
  virtual void preorder_walk(NumberExprAST *ast) override;
  virtual void preorder_walk(StringExprAST *ast) override;
  virtual void preorder_walk(BoolExprAST *ast) override;
  virtual void preorder_walk(VariableExprAST *ast) override;
  virtual void preorder_walk(BlockAST *ast) override;
  virtual void preorder_walk(IfExprAST *ast) override;
  virtual void preorder_walk(UnitExprAST *ast) override;
  virtual void preorder_walk(TypedVarAST *ast) override;
  virtual void preorder_walk(DerefExprAST *ast) override;
  virtual void preorder_walk(AddrOfExprAST *ast) override;
  virtual void preorder_walk(AllocExprAST *ast) override;
  virtual void preorder_walk(FreeExprAST *ast) override;
  virtual void preorder_walk(ArrayLiteralExprAST *ast) override;
  virtual void preorder_walk(IndexExprAST *ast) override;
  virtual void preorder_walk(LenExprAST *ast) override;
  virtual void preorder_walk(UnaryNegExprAST *ast) override;
  virtual void preorder_walk(StructLiteralExprAST *ast) override;
  virtual void preorder_walk(FieldAccessExprAST *ast) override;
  virtual void preorder_walk(TypeClassDeclAST *ast) override;
  virtual void preorder_walk(TypeClassInstanceAST *ast) override;

  // post order
  virtual void postorder_walk(ProgramAST *ast) override;
  virtual void postorder_walk(VarDefAST *ast) override;
  virtual void postorder_walk(ExternAST *ast) override;
  virtual void postorder_walk(FuncDefAST *ast) override;
  virtual void postorder_walk(StructDefAST *ast) override;
  virtual void postorder_walk(PrototypeAST *ast) override;
  virtual void postorder_walk(CallExprAST *ast) override;
  virtual void postorder_walk(ReturnExprAST *ast) override;
  virtual void postorder_walk(BinaryExprAST *ast) override;
  virtual void postorder_walk(NumberExprAST *ast) override;
  virtual void postorder_walk(StringExprAST *ast) override;
  virtual void postorder_walk(BoolExprAST *ast) override;
  virtual void postorder_walk(VariableExprAST *ast) override;
  virtual void postorder_walk(BlockAST *ast) override;
  virtual void postorder_walk(IfExprAST *ast) override;
  virtual void postorder_walk(UnitExprAST *ast) override;
  virtual void postorder_walk(TypedVarAST *ast) override;
  virtual void postorder_walk(DerefExprAST *ast) override;
  virtual void postorder_walk(AddrOfExprAST *ast) override;
  virtual void postorder_walk(AllocExprAST *ast) override;
  virtual void postorder_walk(FreeExprAST *ast) override;
  virtual void postorder_walk(ArrayLiteralExprAST *ast) override;
  virtual void postorder_walk(IndexExprAST *ast) override;
  virtual void postorder_walk(LenExprAST *ast) override;
  virtual void postorder_walk(UnaryNegExprAST *ast) override;
  virtual void postorder_walk(StructLiteralExprAST *ast) override;
  virtual void postorder_walk(FieldAccessExprAST *ast) override;
  virtual void postorder_walk(TypeClassDeclAST *ast) override;
  virtual void postorder_walk(TypeClassInstanceAST *ast) override;

  virtual void visit(TypeClassDeclAST *ast) override;
  virtual void visit(TypeClassInstanceAST *ast) override;

  void safeguard_visit(AstBase *ast, const std::string &msg) {
    if (ast)
      ast->accept_vis(this);
    else
      add_to_rep(fmt::format("{} {}", tabs(), msg));
  }
  friend Printable;
};

void ASTPrinter::print(AstBase *ast) {
  auto vs = AstPrinterVisitor();
  ast->accept_vis(&vs);
}
void ASTPrinter::print(ProgramAST *ast) {
  auto vs = AstPrinterVisitor();
  ast->accept_vis(&vs);
}
std::string_view AstPrinterVisitor::tabs() const { return this->current_tabs; }

void AstPrinterVisitor::visit(ProgramAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  for (auto &def : ast->DefinitionVec) {
    safeguard_visit(def.get(), "!!nullptr!! DefinitionAST\n");
  }
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(VarDefAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->TypedVar.get(), "!!nullptr!! TypedVarAST\n");
  safeguard_visit(ast->Expression.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(ExternAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->Prototype.get(), "!!nullptr!! PrototypeAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(FuncDefAST *ast) {
  generic_preprintln(ast);
  this->enter_new_scope();
  ast->walk_with_preorder(this);
  safeguard_visit(ast->Prototype.get(), "!!nullptr!! PrototypeAST\n");
  safeguard_visit(ast->Block.get(), "!!nullptr!! BlockAST\n");
  ast->walk_with_postorder(this);
  this->exit_new_scope();
  generic_postprint();
}

void AstPrinterVisitor::visit(StructDefAST *ast) {
  generic_preprintln(ast);
  this->enter_new_scope();
  ast->walk_with_preorder(this);
  for (auto &t : ast->struct_members)
    safeguard_visit(t.get(), "!!nullptr!! struct_members (typed var)");
  ast->walk_with_postorder(this);
  this->exit_new_scope();
  generic_postprint();
}

void AstPrinterVisitor::visit(PrototypeAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  for (auto &var : ast->parameterVectors) {
    safeguard_visit(var.get(), "!!nullptr!! TypedVarAST\n");
  }
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::visit(TypedVarAST *ast) {
  add_to_rep(fmt::format("{} {}: ", tabs(), ast->getTreeName()));
  ast->walk_with_preorder(this);
  ast->walk_with_postorder(this);
  add_to_rep("\n");
}

void AstPrinterVisitor::visit(CallExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  for (auto &arg : ast->arguments) {
    safeguard_visit(arg.get(), "!!nullptr!! ExprAST\n");
  }
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(BinaryExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->LHS.get(), "!!nullptr!! ExprAST\n");
  safeguard_visit(ast->RHS.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(StringExprAST *ast) {
  generic_preprint(ast);
  ast->walk_with_preorder(this);
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::visit(NumberExprAST *ast) {
  generic_preprint(ast);
  ast->walk_with_preorder(this);
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(VariableExprAST *ast) {
  generic_preprint(ast);
  ast->walk_with_preorder(this);
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(UnitExprAST *ast) {
  generic_preprint(ast);
  ast->walk_with_preorder(this);
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(IfExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->bool_expr.get(), "!!nullptr!! ExprAST\n");
  safeguard_visit(ast->thenBlockAST.get(), "!!nullptr!! thenBlockAST\n");
  safeguard_visit(ast->elseBlockAST.get(), "!!nullptr!! elseBlockAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(BoolExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::visit(ReturnExprAST *ast) {

  generic_preprint(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->return_expr.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::visit(BlockAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  for (auto &stmt : ast->Statements) {
    safeguard_visit(stmt.get(), "!!nullptr!! ExprAST\n");
  }
  ast->walk_with_postorder(this);
  generic_postprint();
}

void AstPrinterVisitor::generic_preprintln(AstBase *ast) {
  trim_rep();
  if (ast->pe)
    add_to_rep(fmt::format("{} {} \n", tabs(),
                           ast->getTreeName() + " - !!ParserError!!"));
  else
    add_to_rep(fmt::format("{} {} - {}\n", tabs(), ast->getTreeName(),
                           ast->type.to_string()));
  current_tabs += tab;
}
void AstPrinterVisitor::generic_preprint(AstBase *ast) {
  trim_rep();
  if (ast->pe)
    add_to_rep(fmt::format("{} {} : ", tabs(),
                           ast->getTreeName() + " - !!ParserError!!"));
  else
    add_to_rep(fmt::format("{} {} - {} : ", tabs(), ast->getTreeName(),
                           ast->type.to_string()));
  current_tabs += tab;
}
void AstPrinterVisitor::add_to_rep(const std::string &s) {
  trim_rep();
  this->rep += s;
  trim_rep();
}
void AstPrinterVisitor::trim_rep() {
  while (this->rep.size() >= 2) {
    if (this->rep[rep.size() - 1] == '\n' and this->rep[rep.size() - 2] == '\n')
      this->rep.pop_back();
    else
      break;
  }
}

void AstPrinterVisitor::generic_postprint() {
  if (!current_tabs.empty())
    current_tabs.pop_back();
  if (!current_tabs.empty())
    current_tabs.pop_back();
  while (this->rep.size() >= 2) {
    if (this->rep[rep.size() - 1] == '\n' and this->rep[rep.size() - 2] == '\n')
      this->rep.pop_back();
    else
      break;
  }
  if (this->rep.back() != '\n')
    add_to_rep(fmt::format("\n"));
}
void AstPrinterVisitor::preorder_walk(ProgramAST *ast) {}
void AstPrinterVisitor::preorder_walk(VarDefAST *ast) {}
void AstPrinterVisitor::preorder_walk(ExternAST *ast) {}
void AstPrinterVisitor::preorder_walk(FuncDefAST *ast) {}
void AstPrinterVisitor::preorder_walk(StructDefAST *ast) {
  // print the record’s name at the current indent
  add_to_rep(fmt::format("{}struct_name: \"{}\"", tabs(), ast->struct_name.display()));
}
void AstPrinterVisitor::preorder_walk(PrototypeAST *ast) {
  add_to_rep(fmt::format("{} fn_name: \"{}\"\n", tabs(), ast->functionName.display()));
}
void AstPrinterVisitor::preorder_walk(CallExprAST *ast) {}
void AstPrinterVisitor::preorder_walk(ReturnExprAST *ast) {
  add_to_rep(fmt::format("{}\n", ast->is_implicit ? "implicit" : "explicit"));
}
void AstPrinterVisitor::preorder_walk(BinaryExprAST *ast) {}
void AstPrinterVisitor::preorder_walk(StringExprAST *ast) {
  add_to_rep(fmt::format("\"{}\")", ast->string_content));
}
void AstPrinterVisitor::preorder_walk(NumberExprAST *ast) {
  add_to_rep(fmt::format("(num, type): (\"{}\", {})", ast->number,
                         ast->type.to_string()));
}
void AstPrinterVisitor::preorder_walk(BoolExprAST *ast) {}
void AstPrinterVisitor::preorder_walk(VariableExprAST *ast) {
  add_to_rep(fmt::format("(var_name, type): (\"{}\", {})", ast->variableName,
                         ast->type.to_string()));
}
void AstPrinterVisitor::preorder_walk(BlockAST *ast) {}
void AstPrinterVisitor::preorder_walk(IfExprAST *ast) {}
void AstPrinterVisitor::preorder_walk(UnitExprAST *ast) {
  add_to_rep(fmt::format("{}", ast->is_implicit ? "implicit" : "explicit"));
}
void AstPrinterVisitor::preorder_walk(TypedVarAST *ast) {
  auto type_str = ast->type_expr ? ast->type_expr->to_string() : "";
  add_to_rep(fmt::format("(name, type_expr, type): (\"{}\", \"{}\", {})",
                         ast->name, type_str, ast->type.to_string()));
}
//
// post order
void AstPrinterVisitor::postorder_walk(ProgramAST *ast) {
  fmt::print("{}", this->disclaimer);
  while (!this->rep.empty()) {
    auto &ch = this->rep.back();
    if (ch == ' ' || ch == '\n') {
      this->rep.pop_back();
    } else {
      break;
    }
  }
  add_to_rep("\n");

  fmt::print("{}", this->rep);
}

void AstPrinterVisitor::postorder_walk(VarDefAST *ast) {}
void AstPrinterVisitor::postorder_walk(ExternAST *ast) {}
void AstPrinterVisitor::postorder_walk(FuncDefAST *ast) {}
void AstPrinterVisitor::postorder_walk(StructDefAST *ast) {}

void AstPrinterVisitor::postorder_walk(PrototypeAST *ast) {}
void AstPrinterVisitor::postorder_walk(CallExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(ReturnExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(BinaryExprAST *ast) {
  add_to_rep(fmt::format("{} Operator: \"{}\"", tabs(),
                         ast->Op ? ast->Op->lexeme : " - !!ParserError!!"));
}
void AstPrinterVisitor::postorder_walk(NumberExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(StringExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(BoolExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(VariableExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(BlockAST *ast) {}
void AstPrinterVisitor::postorder_walk(IfExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(UnitExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(TypedVarAST *ast) {}

void AstPrinterVisitor::visit(DerefExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::visit(AddrOfExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::preorder_walk(DerefExprAST *ast) {}
void AstPrinterVisitor::preorder_walk(AddrOfExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(DerefExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(AddrOfExprAST *ast) {}

void AstPrinterVisitor::visit(AllocExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::visit(FreeExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::preorder_walk(AllocExprAST *ast) {}
void AstPrinterVisitor::preorder_walk(FreeExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(AllocExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(FreeExprAST *ast) {}

void AstPrinterVisitor::visit(ArrayLiteralExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  for (auto &elem : ast->elements)
    safeguard_visit(elem.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::visit(IndexExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->array_expr.get(), "!!nullptr!! ExprAST\n");
  safeguard_visit(ast->index_expr.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::visit(LenExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::preorder_walk(ArrayLiteralExprAST *ast) {}
void AstPrinterVisitor::preorder_walk(IndexExprAST *ast) {}
void AstPrinterVisitor::preorder_walk(LenExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(ArrayLiteralExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(IndexExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(LenExprAST *ast) {}

void AstPrinterVisitor::visit(UnaryNegExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::preorder_walk(UnaryNegExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(UnaryNegExprAST *ast) {}

void AstPrinterVisitor::visit(StructLiteralExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  for (auto &val : ast->field_values)
    safeguard_visit(val.get(), "!!nullptr!! field_value");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::preorder_walk(StructLiteralExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(StructLiteralExprAST *ast) {}

void AstPrinterVisitor::visit(FieldAccessExprAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  safeguard_visit(ast->object_expr.get(), "!!nullptr!! object_expr");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::preorder_walk(FieldAccessExprAST *ast) {}
void AstPrinterVisitor::postorder_walk(FieldAccessExprAST *ast) {}

void AstPrinterVisitor::visit(TypeClassDeclAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  for (auto &method : ast->methods)
    safeguard_visit(method.get(), "!!nullptr!! PrototypeAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::visit(TypeClassInstanceAST *ast) {
  generic_preprintln(ast);
  ast->walk_with_preorder(this);
  for (auto &method : ast->methods)
    safeguard_visit(method.get(), "!!nullptr!! FuncDefAST\n");
  ast->walk_with_postorder(this);
  generic_postprint();
}
void AstPrinterVisitor::preorder_walk(TypeClassDeclAST *ast) {
  add_to_rep(fmt::format("{}typeclass: {}<{}>\n", tabs(), ast->class_name,
                         ast->type_param));
}
void AstPrinterVisitor::preorder_walk(TypeClassInstanceAST *ast) {
  add_to_rep(fmt::format("{}instance: {}<{}>\n", tabs(), ast->class_name,
                         ast->concrete_type.to_string()));
}
void AstPrinterVisitor::postorder_walk(TypeClassDeclAST *ast) {}
void AstPrinterVisitor::postorder_walk(TypeClassInstanceAST *ast) {}

} // namespace sammine_lang::AST
