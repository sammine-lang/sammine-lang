#include "typecheck/NeverValueCheck.h"
#include "fmt/format.h"

//! \file NeverValueCheck.cpp
//! \brief Each expression typed Never whose value something uses.

namespace sammine_lang::AST {

void NeverValueCheck::check(ExprAST *used, const char *as) {
  if (!used || !used->get_type().is_never())
    return;
  add_error(used->get_location(),
            fmt::format("This never gives a value, so it can't be {}: it "
                        "ends the program or returns first. Make it a "
                        "statement, or a branch's value",
                        as));
}

void NeverValueCheck::check(std::vector<std::unique_ptr<ExprAST>> &used,
                            const char *as) {
  for (auto &expr : used)
    check(expr.get(), as);
}

void NeverValueCheck::visit(FuncDefAST *ast) {
  if (ast->Prototype->is_generic())
    return;
  ASTVisitor::visit(ast);
}

void NeverValueCheck::preorder_walk(CallExprAST *ast) {
  check(ast->arguments, "an argument");
}
void NeverValueCheck::preorder_walk(BinaryExprAST *ast) {
  check(ast->LHS.get(), "an operand");
  check(ast->RHS.get(), "an operand");
}
void NeverValueCheck::preorder_walk(UnaryNegExprAST *ast) {
  check(ast->operand.get(), "an operand");
}
void NeverValueCheck::preorder_walk(DerefExprAST *ast) {
  check(ast->operand.get(), "dereferenced");
}
void NeverValueCheck::preorder_walk(AddrOfExprAST *ast) {
  check(ast->operand.get(), "lent");
}
void NeverValueCheck::preorder_walk(AllocExprAST *ast) {
  check(ast->operand.get(), "a count");
}
void NeverValueCheck::preorder_walk(DropExprAST *ast) {
  check(ast->operand.get(), "dropped");
}
void NeverValueCheck::preorder_walk(CastExprAST *ast) {
  check(ast->operand.get(), "converted");
}
void NeverValueCheck::preorder_walk(ArrayLiteralExprAST *ast) {
  check(ast->elements, "an element");
}
void NeverValueCheck::preorder_walk(RangeExprAST *ast) {
  check(ast->start.get(), "a bound");
  check(ast->end.get(), "a bound");
}
void NeverValueCheck::preorder_walk(IndexExprAST *ast) {
  check(ast->array_expr.get(), "indexed");
  check(ast->index_expr.get(), "an index");
}
void NeverValueCheck::preorder_walk(LenExprAST *ast) {
  check(ast->operand.get(), "measured");
}
void NeverValueCheck::preorder_walk(DimExprAST *ast) {
  check(ast->operand.get(), "measured");
}
void NeverValueCheck::preorder_walk(PanicExprAST *ast) {
  check(ast->arguments, "printed");
}
void NeverValueCheck::preorder_walk(StructLiteralExprAST *ast) {
  check(ast->field_values, "a field");
}
void NeverValueCheck::preorder_walk(FieldAccessExprAST *ast) {
  check(ast->object_expr.get(), "accessed");
}
void NeverValueCheck::preorder_walk(TupleLiteralExprAST *ast) {
  check(ast->elements, "an element");
}
void NeverValueCheck::preorder_walk(IfExprAST *ast) {
  check(ast->bool_expr.get(), "a condition");
}
void NeverValueCheck::preorder_walk(CaseExprAST *ast) {
  check(ast->scrutinee.get(), "matched");
}
void NeverValueCheck::preorder_walk(WhileExprAST *ast) {
  check(ast->condition.get(), "a condition");
}
void NeverValueCheck::preorder_walk(ForExprAST *ast) {
  check(ast->start.get(), "a bound");
  check(ast->end.get(), "a bound");
}

} // namespace sammine_lang::AST
