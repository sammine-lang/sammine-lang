#pragma once

#include "ast/Ast.h"
#include "typecheck/Types.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace sammine_lang::AST {

class Monomorphizer {
public:
  using SubstitutionMap = std::unordered_map<std::string, Type>;

  static std::unique_ptr<FuncDefAST>
  instantiate(FuncDefAST *generic, const std::string &mangled_name,
              const SubstitutionMap &bindings);

private:
  const SubstitutionMap &bindings;
  explicit Monomorphizer(const SubstitutionMap &bindings)
      : bindings(bindings) {}

  std::unique_ptr<TypeExprAST> clone_type_expr(TypeExprAST *expr);
  std::unique_ptr<TypedVarAST> clone_typed_var(TypedVarAST *var);
  std::unique_ptr<PrototypeAST>
  clone_prototype(PrototypeAST *proto, const std::string &mangled_name);
  std::unique_ptr<BlockAST> clone_block(BlockAST *block);
  std::unique_ptr<ExprAST> clone_expr(ExprAST *expr);

  // Helper to get the concrete type name for a type param
  std::string resolve_type_name(const std::string &name) const;
};

} // namespace sammine_lang::AST
