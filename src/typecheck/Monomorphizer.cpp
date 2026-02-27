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

  if (auto *simple = llvm::dyn_cast<SimpleTypeExprAST>(expr)) {
    auto resolved = resolve_type_name(simple->name.mangled());
    return std::make_unique<SimpleTypeExprAST>(make_tok(resolved));
  }

  if (auto *ptr = llvm::dyn_cast<PointerTypeExprAST>(expr)) {
    return std::make_unique<PointerTypeExprAST>(
        clone_type_expr(ptr->pointee.get()));
  }

  if (auto *arr = llvm::dyn_cast<ArrayTypeExprAST>(expr)) {
    return std::make_unique<ArrayTypeExprAST>(
        clone_type_expr(arr->element.get()), arr->size);
  }

  if (auto *fn = llvm::dyn_cast<FunctionTypeExprAST>(expr)) {
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

std::vector<std::unique_ptr<ExprAST>> Monomorphizer::clone_expr_vec(
    const std::vector<std::unique_ptr<ExprAST>> &exprs) {
  std::vector<std::unique_ptr<ExprAST>> result;
  result.reserve(exprs.size());
  for (auto &e : exprs)
    result.push_back(clone_expr(e.get()));
  return result;
}

std::unique_ptr<BlockAST> Monomorphizer::clone_block(BlockAST *block) {
  if (!block)
    return nullptr;
  auto result = std::make_unique<BlockAST>();
  result->Statements = clone_expr_vec(block->Statements);
  return result;
}

// Handled ExprAST subtypes (update when adding new ExprAST nodes):
// NumberExprAST, StringExprAST, BoolExprAST, CharExprAST,
// VariableExprAST, CallExprAST, BinaryExprAST, ReturnExprAST,
// VarDefAST, IfExprAST, UnitExprAST, DerefExprAST, AddrOfExprAST,
// AllocExprAST, FreeExprAST, ArrayLiteralExprAST, IndexExprAST,
// LenExprAST, UnaryNegExprAST, StructLiteralExprAST, FieldAccessExprAST
std::unique_ptr<ExprAST> Monomorphizer::clone_expr(ExprAST *expr) {
  if (!expr)
    return nullptr;

  if (auto *num = llvm::dyn_cast<NumberExprAST>(expr)) {
    auto result = std::make_unique<NumberExprAST>(make_tok(num->number));
    return result;
  }

  if (auto *str = llvm::dyn_cast<StringExprAST>(expr)) {
    auto tok = make_tok(str->string_content);
    tok->tok_type = TokenType::TokStr;
    auto result = std::make_unique<StringExprAST>(tok);
    return result;
  }

  if (auto *b = llvm::dyn_cast<BoolExprAST>(expr)) {
    auto result = std::make_unique<BoolExprAST>(b->b, b->get_location());
    return result;
  }

  if (auto *ch = llvm::dyn_cast<CharExprAST>(expr)) {
    return std::make_unique<CharExprAST>(ch->value, ch->get_location());
  }

  if (auto *var = llvm::dyn_cast<VariableExprAST>(expr)) {
    auto result =
        std::make_unique<VariableExprAST>(make_tok(var->variableName));
    return result;
  }

  if (auto *call = llvm::dyn_cast<CallExprAST>(expr)) {
    auto result = std::make_unique<CallExprAST>(
        call->functionName, call->get_location(),
        clone_expr_vec(call->arguments));
    for (auto &ta : call->explicit_type_args)
      result->explicit_type_args.push_back(clone_type_expr(ta.get()));
    return result;
  }

  if (auto *bin = llvm::dyn_cast<BinaryExprAST>(expr)) {
    auto result = std::make_unique<BinaryExprAST>(
        bin->Op, clone_expr(bin->LHS.get()), clone_expr(bin->RHS.get()));
    return result;
  }

  if (auto *ret = llvm::dyn_cast<ReturnExprAST>(expr)) {
    if (ret->is_implicit) {
      auto result =
          std::make_unique<ReturnExprAST>(clone_expr(ret->return_expr.get()));
      return result;
    }
    auto result = std::make_unique<ReturnExprAST>(
        make_tok("return"), clone_expr(ret->return_expr.get()));
    return result;
  }

  if (auto *vd = llvm::dyn_cast<VarDefAST>(expr)) {
    auto result = std::make_unique<VarDefAST>(
        make_tok("let"), clone_typed_var(vd->TypedVar.get()),
        clone_expr(vd->Expression.get()), vd->is_mutable);
    return result;
  }

  if (auto *ife = llvm::dyn_cast<IfExprAST>(expr)) {
    auto result = std::make_unique<IfExprAST>(
        clone_expr(ife->bool_expr.get()), clone_block(ife->thenBlockAST.get()),
        clone_block(ife->elseBlockAST.get()));
    return result;
  }

  if (auto *unit = llvm::dyn_cast<UnitExprAST>(expr)) {
    if (unit->is_implicit)
      return std::make_unique<UnitExprAST>();
    return std::make_unique<UnitExprAST>(make_tok("("), make_tok(")"));
  }

  if (auto *deref = llvm::dyn_cast<DerefExprAST>(expr)) {
    return std::make_unique<DerefExprAST>(make_tok("*"),
                                          clone_expr(deref->operand.get()));
  }

  if (auto *addr = llvm::dyn_cast<AddrOfExprAST>(expr)) {
    return std::make_unique<AddrOfExprAST>(make_tok("&"),
                                           clone_expr(addr->operand.get()));
  }

  if (auto *alloc = llvm::dyn_cast<AllocExprAST>(expr)) {
    return std::make_unique<AllocExprAST>(make_tok("alloc"),
                                          clone_type_expr(alloc->type_arg.get()),
                                          clone_expr(alloc->operand.get()));
  }

  if (auto *free_e = llvm::dyn_cast<FreeExprAST>(expr)) {
    return std::make_unique<FreeExprAST>(make_tok("free"),
                                         clone_expr(free_e->operand.get()));
  }

  if (auto *arr_lit = llvm::dyn_cast<ArrayLiteralExprAST>(expr)) {
    return std::make_unique<ArrayLiteralExprAST>(
        clone_expr_vec(arr_lit->elements));
  }

  if (auto *idx = llvm::dyn_cast<IndexExprAST>(expr)) {
    return std::make_unique<IndexExprAST>(clone_expr(idx->array_expr.get()),
                                          clone_expr(idx->index_expr.get()));
  }

  if (auto *len = llvm::dyn_cast<LenExprAST>(expr)) {
    return std::make_unique<LenExprAST>(make_tok("len"),
                                        clone_expr(len->operand.get()));
  }

  if (auto *neg = llvm::dyn_cast<UnaryNegExprAST>(expr)) {
    return std::make_unique<UnaryNegExprAST>(make_tok("-"),
                                             clone_expr(neg->operand.get()));
  }

  if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(expr)) {
    return std::make_unique<StructLiteralExprAST>(
        sl->struct_name, sl->get_location(), sl->field_names,
        clone_expr_vec(sl->field_values));
  }

  if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(expr)) {
    return std::make_unique<FieldAccessExprAST>(
        clone_expr(fa->object_expr.get()), make_tok(fa->field_name));
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
