#include "typecheck/Monomorphizer.h"
#include "ast/Ast.h"
#include "lex/Token.h"
#include "util/Utilities.h"

namespace sammine_lang::AST {

static std::shared_ptr<Token> make_tok(const std::string &lexeme) {
  return std::make_shared<Token>(TokenType::TokID, lexeme,
                                 sammine_util::Location{});
}

std::string Monomorphizer::resolve_type_name(const std::string &name) const {
  auto it = bindings.find(name);
  if (it != bindings.end())
    return it->second.to_string();
  return name;
}

std::unique_ptr<TypeExprAST> Monomorphizer::clone_type_expr(TypeExprAST *expr) {
  if (!expr)
    return nullptr;

  if (auto *simple = dynamic_cast<SimpleTypeExprAST *>(expr)) {
    auto resolved = resolve_type_name(simple->name);
    return std::make_unique<SimpleTypeExprAST>(make_tok(resolved));
  }

  if (auto *ptr = dynamic_cast<PointerTypeExprAST *>(expr)) {
    return std::make_unique<PointerTypeExprAST>(
        clone_type_expr(ptr->pointee.get()));
  }

  if (auto *arr = dynamic_cast<ArrayTypeExprAST *>(expr)) {
    return std::make_unique<ArrayTypeExprAST>(
        clone_type_expr(arr->element.get()), arr->size);
  }

  if (auto *fn = dynamic_cast<FunctionTypeExprAST *>(expr)) {
    std::vector<std::unique_ptr<TypeExprAST>> params;
    for (auto &p : fn->paramTypes)
      params.push_back(clone_type_expr(p.get()));
    return std::make_unique<FunctionTypeExprAST>(
        std::move(params), clone_type_expr(fn->returnType.get()));
  }

  sammine_util::abort("Unknown TypeExprAST subclass in Monomorphizer");
}

std::unique_ptr<TypedVarAST> Monomorphizer::clone_typed_var(TypedVarAST *var) {
  if (!var)
    return nullptr;
  auto result = std::make_unique<TypedVarAST>(
      make_tok(var->name), clone_type_expr(var->type_expr.get()),
      var->is_mutable);
  return result;
}

std::unique_ptr<PrototypeAST>
Monomorphizer::clone_prototype(PrototypeAST *proto,
                               const std::string &mangled_name) {
  std::vector<std::unique_ptr<TypedVarAST>> params;
  for (auto &p : proto->parameterVectors)
    params.push_back(clone_typed_var(p.get()));

  auto result = std::make_unique<PrototypeAST>(
      make_tok(mangled_name), clone_type_expr(proto->return_type_expr.get()),
      std::move(params));
  // type_params left empty — this is a concrete instantiation
  return result;
}

std::unique_ptr<BlockAST> Monomorphizer::clone_block(BlockAST *block) {
  if (!block)
    return nullptr;
  auto result = std::make_unique<BlockAST>();
  for (auto &stmt : block->Statements)
    result->Statements.push_back(clone_expr(stmt.get()));
  return result;
}

std::unique_ptr<ExprAST> Monomorphizer::clone_expr(ExprAST *expr) {
  if (!expr)
    return nullptr;

  if (auto *num = dynamic_cast<NumberExprAST *>(expr)) {
    auto result = std::make_unique<NumberExprAST>(make_tok(num->number));
    return result;
  }

  if (auto *str = dynamic_cast<StringExprAST *>(expr)) {
    auto tok = make_tok(str->string_content);
    tok->tok_type = TokenType::TokStr;
    auto result = std::make_unique<StringExprAST>(tok);
    return result;
  }

  if (auto *b = dynamic_cast<BoolExprAST *>(expr)) {
    auto result = std::make_unique<BoolExprAST>(b->b, b->get_location());
    return result;
  }

  if (auto *var = dynamic_cast<VariableExprAST *>(expr)) {
    auto result =
        std::make_unique<VariableExprAST>(make_tok(var->variableName));
    return result;
  }

  if (auto *call = dynamic_cast<CallExprAST *>(expr)) {
    std::vector<std::unique_ptr<ExprAST>> args;
    for (auto &a : call->arguments)
      args.push_back(clone_expr(a.get()));
    auto result = std::make_unique<CallExprAST>(make_tok(call->functionName),
                                                std::move(args));
    return result;
  }

  if (auto *bin = dynamic_cast<BinaryExprAST *>(expr)) {
    auto result = std::make_unique<BinaryExprAST>(
        bin->Op, clone_expr(bin->LHS.get()), clone_expr(bin->RHS.get()));
    return result;
  }

  if (auto *ret = dynamic_cast<ReturnExprAST *>(expr)) {
    if (ret->is_implicit) {
      auto result =
          std::make_unique<ReturnExprAST>(clone_expr(ret->return_expr.get()));
      return result;
    }
    auto result = std::make_unique<ReturnExprAST>(
        make_tok("return"), clone_expr(ret->return_expr.get()));
    return result;
  }

  if (auto *vd = dynamic_cast<VarDefAST *>(expr)) {
    auto result = std::make_unique<VarDefAST>(
        make_tok("let"), clone_typed_var(vd->TypedVar.get()),
        clone_expr(vd->Expression.get()), vd->is_mutable);
    return result;
  }

  if (auto *ife = dynamic_cast<IfExprAST *>(expr)) {
    auto result = std::make_unique<IfExprAST>(
        clone_expr(ife->bool_expr.get()), clone_block(ife->thenBlockAST.get()),
        clone_block(ife->elseBlockAST.get()));
    return result;
  }

  if (auto *unit = dynamic_cast<UnitExprAST *>(expr)) {
    if (unit->is_implicit)
      return std::make_unique<UnitExprAST>();
    return std::make_unique<UnitExprAST>(make_tok("("), make_tok(")"));
  }

  if (auto *deref = dynamic_cast<DerefExprAST *>(expr)) {
    return std::make_unique<DerefExprAST>(make_tok("*"),
                                          clone_expr(deref->operand.get()));
  }

  if (auto *addr = dynamic_cast<AddrOfExprAST *>(expr)) {
    return std::make_unique<AddrOfExprAST>(make_tok("&"),
                                           clone_expr(addr->operand.get()));
  }

  if (auto *alloc = dynamic_cast<AllocExprAST *>(expr)) {
    return std::make_unique<AllocExprAST>(make_tok("alloc"),
                                          clone_expr(alloc->operand.get()));
  }

  if (auto *free_e = dynamic_cast<FreeExprAST *>(expr)) {
    return std::make_unique<FreeExprAST>(make_tok("free"),
                                         clone_expr(free_e->operand.get()));
  }

  if (auto *arr_lit = dynamic_cast<ArrayLiteralExprAST *>(expr)) {
    std::vector<std::unique_ptr<ExprAST>> elements;
    for (auto &e : arr_lit->elements)
      elements.push_back(clone_expr(e.get()));
    return std::make_unique<ArrayLiteralExprAST>(std::move(elements));
  }

  if (auto *idx = dynamic_cast<IndexExprAST *>(expr)) {
    return std::make_unique<IndexExprAST>(clone_expr(idx->array_expr.get()),
                                          clone_expr(idx->index_expr.get()));
  }

  if (auto *len = dynamic_cast<LenExprAST *>(expr)) {
    return std::make_unique<LenExprAST>(make_tok("len"),
                                        clone_expr(len->operand.get()));
  }

  if (auto *neg = dynamic_cast<UnaryNegExprAST *>(expr)) {
    return std::make_unique<UnaryNegExprAST>(make_tok("-"),
                                             clone_expr(neg->operand.get()));
  }

  sammine_util::abort("Unknown ExprAST subclass in Monomorphizer");
}

std::unique_ptr<FuncDefAST>
Monomorphizer::instantiate(FuncDefAST *generic, const std::string &mangled_name,
                           const SubstitutionMap &bindings) {
  Monomorphizer m(bindings);
  auto proto = m.clone_prototype(generic->Prototype.get(), mangled_name);
  auto block = m.clone_block(generic->Block.get());
  return std::make_unique<FuncDefAST>(std::move(proto), std::move(block));
}

} // namespace sammine_lang::AST
