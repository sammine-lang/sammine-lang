#pragma once
#include <set>
#include <string>

namespace sammine_lang::AST {

class ExprAST;
class TypeExprAST;

void collect_call_names(const ExprAST *expr, std::set<std::string> &names);
void collect_type_names(const TypeExprAST *expr, std::set<std::string> &names);
void collect_expr_type_names(const ExprAST *expr,
                             std::set<std::string> &names);

} // namespace sammine_lang::AST
