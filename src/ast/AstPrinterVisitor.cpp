#include "ast/Ast.h"
#include "ast/ASTProperties.h"
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
  virtual void enter_new_scope() override {
    push_ast_context();
    current_tabs += tab;
  }
  virtual void exit_new_scope() override {
    current_tabs.pop_back();
    current_tabs.pop_back();
    pop_ast_context();
  }

  virtual void visit(ProgramAST *ast) override;
  virtual void visit(VarDefAST *ast) override;
  virtual void visit(ExternAST *ast) override;
  virtual void visit(FuncDefAST *ast) override;
  virtual void visit(StructDefAST *ast) override;
  virtual void visit(EnumDefAST *ast) override;
  virtual void visit(TypeAliasDefAST *ast) override;
  virtual void visit(PrototypeAST *ast) override;
  virtual void visit(CallExprAST *ast) override;
  virtual void visit(BinaryExprAST *ast) override;
  virtual void visit(NumberExprAST *ast) override;
  virtual void visit(StringExprAST *ast) override;
  virtual void visit(BoolExprAST *ast) override;
  virtual void visit(VariableExprAST *ast) override;
  virtual void visit(BlockAST *ast) override;
  virtual void visit(ReturnStmtAST *ast) override;
  virtual void visit(IfExprAST *ast) override;
  virtual void visit(UnitExprAST *ast) override;
  virtual void visit(TypedVarAST *ast) override;
  virtual void visit(DerefExprAST *ast) override;
  virtual void visit(AddrOfExprAST *ast) override;
  virtual void visit(AllocExprAST *ast) override;
  virtual void visit(FreeExprAST *ast) override;
  virtual void visit(ArrayLiteralExprAST *ast) override;
  virtual void visit(RangeExprAST *ast) override;
  virtual void visit(IndexExprAST *ast) override;
  virtual void visit(LenExprAST *ast) override;
  virtual void visit(DimExprAST *ast) override;
  virtual void visit(UnaryNegExprAST *ast) override;
  virtual void visit(StructLiteralExprAST *ast) override;
  virtual void visit(FieldAccessExprAST *ast) override;
  virtual void visit(CaseExprAST *ast) override;
  virtual void visit(WhileExprAST *ast) override;
  virtual void visit(TupleLiteralExprAST *ast) override;
  virtual void visit(TypeClassDeclAST *ast) override;
  virtual void visit(TypeClassInstanceAST *ast) override;
  virtual void visit(KernelDefAST *ast) override;

  void safeguard_visit(AstBase *ast, const std::string &msg) {
    if (ast)
      ast->accept_vis(this);
    else
      add_to_rep(fmt::format("{} {}", tabs(), msg));
  }
  friend Printable;
};

void ASTPrinter::print(AstBase *ast, const ASTProperties &props) {
  (void)props; // Will be used in later phases
  auto vs = AstPrinterVisitor();
  ast->accept_vis(&vs);
}
void ASTPrinter::print(ProgramAST *ast, const ASTProperties &props) {
  (void)props; // Will be used in later phases
  auto vs = AstPrinterVisitor();
  ast->accept_vis(&vs);
}
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
  for (auto &def : ast->DefinitionVec) {
    safeguard_visit(def.get(), "!!nullptr!! DefinitionAST\n");
  }
  // Print the AST
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
  generic_postprint();
}

void AstPrinterVisitor::visit(VarDefAST *ast) {
  generic_preprintln(ast);
  if (ast->is_tuple_destructure) {
    for (auto &v : ast->destructure_vars)
      safeguard_visit(v.get(), "!!nullptr!! TypedVarAST\n");
  } else {
    safeguard_visit(ast->TypedVar.get(), "!!nullptr!! TypedVarAST\n");
  }
  safeguard_visit(ast->Expression.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(ExternAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->Prototype.get(), "!!nullptr!! PrototypeAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(FuncDefAST *ast) {
  generic_preprintln(ast);
  this->enter_new_scope();
  safeguard_visit(ast->Prototype.get(), "!!nullptr!! PrototypeAST\n");
  safeguard_visit(ast->Block.get(), "!!nullptr!! BlockAST\n");
  this->exit_new_scope();
  generic_postprint();
}

void AstPrinterVisitor::visit(StructDefAST *ast) {
  generic_preprintln(ast);
  this->enter_new_scope();
  add_to_rep(fmt::format("{}struct_name: \"{}\"", tabs(), ast->struct_name.mangled()));
  for (auto &t : ast->struct_members)
    safeguard_visit(t.get(), "!!nullptr!! struct_members (typed var)");
  this->exit_new_scope();
  generic_postprint();
}

void AstPrinterVisitor::visit(EnumDefAST *ast) {
  generic_preprintln(ast);
  this->enter_new_scope();
  std::string variants_str;
  for (size_t i = 0; i < ast->variants.size(); i++) {
    variants_str += ast->variants[i].name;
    if (!ast->variants[i].payload_types.empty()) {
      variants_str += "(";
      for (size_t j = 0; j < ast->variants[i].payload_types.size(); j++) {
        variants_str += ast->variants[i].payload_types[j]->to_string();
        if (j + 1 < ast->variants[i].payload_types.size())
          variants_str += ", ";
      }
      variants_str += ")";
    }
    if (i + 1 < ast->variants.size())
      variants_str += " | ";
  }
  add_to_rep(fmt::format("{}type_name: \"{}\" = {}", tabs(), ast->enum_name.mangled(), variants_str));
  this->exit_new_scope();
  generic_postprint();
}

void AstPrinterVisitor::visit(TypeAliasDefAST *ast) {
  generic_preprintln(ast);
  add_to_rep(fmt::format("{}type_alias: \"{}\" = {}", tabs(), ast->alias_name.mangled(), ast->type_expr->to_string()));
  generic_postprint();
}

void AstPrinterVisitor::visit(PrototypeAST *ast) {
  generic_preprintln(ast);
  add_to_rep(fmt::format("{} fn_name: \"{}\"\n", tabs(), ast->functionName.mangled()));
  for (auto &var : ast->parameterVectors) {
    safeguard_visit(var.get(), "!!nullptr!! TypedVarAST\n");
  }
  generic_postprint();
}

void AstPrinterVisitor::visit(TypedVarAST *ast) {
  add_to_rep(fmt::format("{} {}: ", tabs(), ast->getTreeName()));
  auto type_str = ast->type_expr ? ast->type_expr->to_string() : "";
  add_to_rep(fmt::format("(name, type_expr, type): (\"{}\", \"{}\", {})",
                         ast->name, type_str, ast->get_type().to_string()));
  add_to_rep("\n");
}

void AstPrinterVisitor::visit(CallExprAST *ast) {
  generic_preprintln(ast);
  for (auto &arg : ast->arguments) {
    safeguard_visit(arg.get(), "!!nullptr!! ExprAST\n");
  }
  generic_postprint();
}

void AstPrinterVisitor::visit(BinaryExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->LHS.get(), "!!nullptr!! ExprAST\n");
  safeguard_visit(ast->RHS.get(), "!!nullptr!! ExprAST\n");
  add_to_rep(fmt::format("{} Operator: \"{}\"", tabs(),
                         ast->Op ? ast->Op->lexeme : " - !!ParserError!!"));
  generic_postprint();
}

void AstPrinterVisitor::visit(StringExprAST *ast) {
  generic_preprint(ast);
  add_to_rep(fmt::format("\"{}\")", ast->string_content));
  generic_postprint();
}

void AstPrinterVisitor::visit(NumberExprAST *ast) {
  generic_preprint(ast);
  add_to_rep(fmt::format("(num, type): (\"{}\", {})", ast->number,
                         ast->get_type().to_string()));
  generic_postprint();
}

void AstPrinterVisitor::visit(VariableExprAST *ast) {
  generic_preprint(ast);
  add_to_rep(fmt::format("(var_name, type): (\"{}\", {})", ast->variableName,
                         ast->get_type().to_string()));
  generic_postprint();
}

void AstPrinterVisitor::visit(UnitExprAST *ast) {
  generic_preprint(ast);
  add_to_rep(fmt::format("{}", ast->is_implicit ? "implicit" : "explicit"));
  generic_postprint();
}

void AstPrinterVisitor::visit(IfExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->bool_expr.get(), "!!nullptr!! ExprAST\n");
  safeguard_visit(ast->thenBlockAST.get(), "!!nullptr!! thenBlockAST\n");
  safeguard_visit(ast->elseBlockAST.get(), "!!nullptr!! elseBlockAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(BoolExprAST *ast) {
  generic_preprintln(ast);
  generic_postprint();
}

void AstPrinterVisitor::visit(ReturnStmtAST *ast) {
  generic_preprint(ast);
  add_to_rep(fmt::format("{}\n", ast->is_implicit ? "implicit" : "explicit"));
  safeguard_visit(ast->return_expr.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(BlockAST *ast) {
  generic_preprintln(ast);
  for (auto &stmt : ast->Statements) {
    safeguard_visit(stmt.get(), "!!nullptr!! ExprAST\n");
  }
  generic_postprint();
}

void AstPrinterVisitor::generic_preprintln(AstBase *ast) {
  trim_rep();
  if (ast->pe)
    add_to_rep(fmt::format("{} {} \n", tabs(),
                           ast->getTreeName() + " - !!ParserError!!"));
  else
    add_to_rep(fmt::format("{} {} - {}\n", tabs(), ast->getTreeName(),
                           ast->get_type().to_string()));
  current_tabs += tab;
}
void AstPrinterVisitor::generic_preprint(AstBase *ast) {
  trim_rep();
  if (ast->pe)
    add_to_rep(fmt::format("{} {} : ", tabs(),
                           ast->getTreeName() + " - !!ParserError!!"));
  else
    add_to_rep(fmt::format("{} {} - {} : ", tabs(), ast->getTreeName(),
                           ast->get_type().to_string()));
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

void AstPrinterVisitor::visit(DerefExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(AddrOfExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(AllocExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(FreeExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(ArrayLiteralExprAST *ast) {
  generic_preprintln(ast);
  for (auto &elem : ast->elements)
    safeguard_visit(elem.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(RangeExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->start.get(), "!!nullptr!! ExprAST\n");
  safeguard_visit(ast->end.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(IndexExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->array_expr.get(), "!!nullptr!! ExprAST\n");
  safeguard_visit(ast->index_expr.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(LenExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(DimExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(UnaryNegExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->operand.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(StructLiteralExprAST *ast) {
  generic_preprintln(ast);
  for (auto &val : ast->field_values)
    safeguard_visit(val.get(), "!!nullptr!! field_value");
  generic_postprint();
}

void AstPrinterVisitor::visit(FieldAccessExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->object_expr.get(), "!!nullptr!! object_expr");
  generic_postprint();
}

void AstPrinterVisitor::visit(CaseExprAST *ast) {
  generic_preprintln(ast);
  std::string arms_str;
  for (size_t i = 0; i < ast->arms.size(); i++) {
    auto &arm = ast->arms[i];
    if (arm.pattern.is_wildcard)
      arms_str += "_";
    else if (arm.pattern.is_literal)
      arms_str += arm.pattern.literal_value;
    else {
      arms_str += arm.pattern.variant_name.mangled();
      if (!arm.pattern.bindings.empty()) {
        arms_str += "(";
        for (size_t j = 0; j < arm.pattern.bindings.size(); j++) {
          arms_str += arm.pattern.bindings[j];
          if (j + 1 < arm.pattern.bindings.size())
            arms_str += ", ";
        }
        arms_str += ")";
      }
    }
    if (i + 1 < ast->arms.size())
      arms_str += " | ";
  }
  add_to_rep(fmt::format("{}patterns: {}\n", tabs(), arms_str));
  safeguard_visit(ast->scrutinee.get(), "!!nullptr!! scrutinee");
  for (auto &arm : ast->arms)
    safeguard_visit(arm.body.get(), "!!nullptr!! arm body");
  generic_postprint();
}

void AstPrinterVisitor::visit(WhileExprAST *ast) {
  generic_preprintln(ast);
  safeguard_visit(ast->condition.get(), "!!nullptr!! condition\n");
  safeguard_visit(ast->body.get(), "!!nullptr!! body\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(TupleLiteralExprAST *ast) {
  generic_preprintln(ast);
  for (auto &elem : ast->elements)
    safeguard_visit(elem.get(), "!!nullptr!! ExprAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(TypeClassDeclAST *ast) {
  generic_preprintln(ast);
  std::string params;
  for (size_t i = 0; i < ast->type_params.size(); i++) {
    if (i > 0)
      params += ", ";
    params += ast->type_params[i];
  }
  add_to_rep(
      fmt::format("{}typeclass: {}<{}>\n", tabs(), ast->class_name, params));
  for (auto &method : ast->methods)
    safeguard_visit(method.get(), "!!nullptr!! PrototypeAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(TypeClassInstanceAST *ast) {
  generic_preprintln(ast);
  std::string types;
  for (size_t i = 0; i < ast->concrete_type_exprs.size(); i++) {
    if (i > 0)
      types += ", ";
    types += ast->concrete_type_exprs[i]
                 ? ast->concrete_type_exprs[i]->to_string()
                 : "?";
  }
  add_to_rep(
      fmt::format("{}instance: {}<{}>\n", tabs(), ast->class_name, types));
  for (auto &method : ast->methods)
    safeguard_visit(method.get(), "!!nullptr!! FuncDefAST\n");
  generic_postprint();
}

void AstPrinterVisitor::visit(KernelDefAST *ast) {
  generic_preprintln(ast);
  add_to_rep(fmt::format("{}kernel {}\n", tabs(), ast->Prototype->functionName.mangled()));
  safeguard_visit(ast->Prototype.get(), "!!nullptr!! PrototypeAST\n");
  generic_postprint();
}

} // namespace sammine_lang::AST
