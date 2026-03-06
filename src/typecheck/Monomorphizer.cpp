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

  switch (expr->getKind()) {
  case ParseKind::Simple: {
    auto *simple = llvm::cast<SimpleTypeExprAST>(expr);
    auto resolved = resolve_type_name(simple->name.mangled());
    return std::make_unique<SimpleTypeExprAST>(make_tok(resolved));
  }
  case ParseKind::Pointer: {
    auto *ptr = llvm::cast<PointerTypeExprAST>(expr);
    return std::make_unique<PointerTypeExprAST>(
        clone_type_expr(ptr->pointee.get()), ptr->is_linear);
  }
  case ParseKind::Array: {
    auto *arr = llvm::cast<ArrayTypeExprAST>(expr);
    return std::make_unique<ArrayTypeExprAST>(
        clone_type_expr(arr->element.get()), arr->size);
  }
  case ParseKind::Function: {
    auto *fn = llvm::cast<FunctionTypeExprAST>(expr);
    std::vector<std::unique_ptr<TypeExprAST>> params;
    for (auto &p : fn->paramTypes)
      params.push_back(clone_type_expr(p.get()));
    return std::make_unique<FunctionTypeExprAST>(
        std::move(params), clone_type_expr(fn->returnType.get()));
  }
  case ParseKind::Generic: {
    auto *gen = llvm::cast<GenericTypeExprAST>(expr);
    std::vector<std::unique_ptr<TypeExprAST>> cloned_args;
    for (auto &arg : gen->type_args)
      cloned_args.push_back(clone_type_expr(arg.get()));
    return std::make_unique<GenericTypeExprAST>(
        gen->base_name, std::move(cloned_args), gen->location);
  }
  case ParseKind::Tuple: {
    auto *tup = llvm::cast<TupleTypeExprAST>(expr);
    std::vector<std::unique_ptr<TypeExprAST>> cloned_elements;
    for (auto &elem : tup->element_types)
      cloned_elements.push_back(clone_type_expr(elem.get()));
    return std::make_unique<TupleTypeExprAST>(std::move(cloned_elements));
  }
  }
}

std::unique_ptr<TypedVarAST> Monomorphizer::clone_typed_var(TypedVarAST *var) {
  if (!var)
    return nullptr;
  auto result = std::make_unique<TypedVarAST>(
      make_tok(var->name), clone_type_expr(var->type_expr.get()),
      var->is_mutable);
  result->set_location(var->get_location());
  return result;
}

std::unique_ptr<PrototypeAST>
Monomorphizer::clone_prototype(
    PrototypeAST *proto,
    const sammine_util::MonomorphizedName &mono_name) {
  std::vector<std::unique_ptr<TypedVarAST>> params;
  for (auto &p : proto->parameterVectors)
    params.push_back(clone_typed_var(p.get()));

  auto result = std::make_unique<PrototypeAST>(
      mono_name.to_qualified_name(), proto->get_location(),
      clone_type_expr(proto->return_type_expr.get()), std::move(params));
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
  result->set_location(block->get_location());
  return result;
}

// Handled ExprAST subtypes (update when adding new ExprAST nodes):
// NumberExprAST, StringExprAST, BoolExprAST, CharExprAST,
// VariableExprAST, CallExprAST, BinaryExprAST, ReturnExprAST,
// VarDefAST, IfExprAST, UnitExprAST, DerefExprAST, AddrOfExprAST,
// AllocExprAST, FreeExprAST, ArrayLiteralExprAST, IndexExprAST,
// LenExprAST, UnaryNegExprAST, StructLiteralExprAST, FieldAccessExprAST,
// CaseExprAST, WhileExprAST, TupleLiteralExprAST
std::unique_ptr<ExprAST> Monomorphizer::clone_expr(ExprAST *expr) {
  if (!expr)
    return nullptr;

  auto orig_loc = expr->get_location();
  std::unique_ptr<ExprAST> result;

  if (auto *num = llvm::dyn_cast<NumberExprAST>(expr)) {
    result = std::make_unique<NumberExprAST>(make_tok(num->number));
  } else if (auto *str = llvm::dyn_cast<StringExprAST>(expr)) {
    auto tok = make_tok(str->string_content);
    tok->tok_type = TokenType::TokStr;
    result = std::make_unique<StringExprAST>(tok);
  } else if (auto *b = llvm::dyn_cast<BoolExprAST>(expr)) {
    result = std::make_unique<BoolExprAST>(b->b, b->get_location());
  } else if (auto *ch = llvm::dyn_cast<CharExprAST>(expr)) {
    result = std::make_unique<CharExprAST>(ch->value, ch->get_location());
  } else if (auto *var = llvm::dyn_cast<VariableExprAST>(expr)) {
    result = std::make_unique<VariableExprAST>(make_tok(var->variableName));
  } else if (auto *call = llvm::dyn_cast<CallExprAST>(expr)) {
    auto r = std::make_unique<CallExprAST>(
        call->functionName, call->get_location(),
        clone_expr_vec(call->arguments));
    for (auto &ta : call->explicit_type_args)
      r->explicit_type_args.push_back(clone_type_expr(ta.get()));
    result = std::move(r);
  } else if (auto *bin = llvm::dyn_cast<BinaryExprAST>(expr)) {
    result = std::make_unique<BinaryExprAST>(
        bin->Op, clone_expr(bin->LHS.get()), clone_expr(bin->RHS.get()));
  } else if (auto *ret = llvm::dyn_cast<ReturnExprAST>(expr)) {
    if (ret->is_implicit)
      result = std::make_unique<ReturnExprAST>(clone_expr(ret->return_expr.get()));
    else
      result = std::make_unique<ReturnExprAST>(
          make_tok("return"), clone_expr(ret->return_expr.get()));
  } else if (auto *vd = llvm::dyn_cast<VarDefAST>(expr)) {
    if (vd->is_tuple_destructure) {
      std::vector<std::unique_ptr<TypedVarAST>> vars;
      for (auto &v : vd->destructure_vars)
        vars.push_back(clone_typed_var(v.get()));
      result = std::make_unique<VarDefAST>(
          make_tok("let"), std::move(vars),
          clone_expr(vd->Expression.get()), vd->is_mutable);
    } else {
      result = std::make_unique<VarDefAST>(
          make_tok("let"), clone_typed_var(vd->TypedVar.get()),
          clone_expr(vd->Expression.get()), vd->is_mutable);
    }
  } else if (auto *ife = llvm::dyn_cast<IfExprAST>(expr)) {
    result = std::make_unique<IfExprAST>(
        clone_expr(ife->bool_expr.get()), clone_block(ife->thenBlockAST.get()),
        clone_block(ife->elseBlockAST.get()));
  } else if (auto *unit = llvm::dyn_cast<UnitExprAST>(expr)) {
    if (unit->is_implicit)
      result = std::make_unique<UnitExprAST>();
    else
      result = std::make_unique<UnitExprAST>(make_tok("("), make_tok(")"));
  } else if (auto *deref = llvm::dyn_cast<DerefExprAST>(expr)) {
    result = std::make_unique<DerefExprAST>(make_tok("*"),
                                            clone_expr(deref->operand.get()));
  } else if (auto *addr = llvm::dyn_cast<AddrOfExprAST>(expr)) {
    result = std::make_unique<AddrOfExprAST>(make_tok("&"),
                                             clone_expr(addr->operand.get()));
  } else if (auto *alloc = llvm::dyn_cast<AllocExprAST>(expr)) {
    result = std::make_unique<AllocExprAST>(make_tok("alloc"),
                                            clone_type_expr(alloc->type_arg.get()),
                                            clone_expr(alloc->operand.get()));
  } else if (auto *free_e = llvm::dyn_cast<FreeExprAST>(expr)) {
    result = std::make_unique<FreeExprAST>(make_tok("free"),
                                           clone_expr(free_e->operand.get()));
  } else if (auto *arr_lit = llvm::dyn_cast<ArrayLiteralExprAST>(expr)) {
    result = std::make_unique<ArrayLiteralExprAST>(
        clone_expr_vec(arr_lit->elements));
  } else if (auto *idx = llvm::dyn_cast<IndexExprAST>(expr)) {
    result = std::make_unique<IndexExprAST>(clone_expr(idx->array_expr.get()),
                                            clone_expr(idx->index_expr.get()));
  } else if (auto *len = llvm::dyn_cast<LenExprAST>(expr)) {
    result = std::make_unique<LenExprAST>(make_tok("len"),
                                          clone_expr(len->operand.get()));
  } else if (auto *neg = llvm::dyn_cast<UnaryNegExprAST>(expr)) {
    result = std::make_unique<UnaryNegExprAST>(make_tok("-"),
                                               clone_expr(neg->operand.get()));
  } else if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(expr)) {
    auto cloned_sl = std::make_unique<StructLiteralExprAST>(
        sl->struct_name, sl->get_location(), sl->field_names,
        clone_expr_vec(sl->field_values));
    for (auto &ta : sl->explicit_type_args)
      cloned_sl->explicit_type_args.push_back(clone_type_expr(ta.get()));
    result = std::move(cloned_sl);
  } else if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(expr)) {
    result = std::make_unique<FieldAccessExprAST>(
        clone_expr(fa->object_expr.get()), make_tok(fa->field_name));
  } else if (auto *ce = llvm::dyn_cast<CaseExprAST>(expr)) {
    std::vector<CaseArm> cloned_arms;
    for (auto &arm : ce->arms) {
      CaseArm cloned_arm;
      cloned_arm.pattern = arm.pattern;
      cloned_arm.body = clone_block(arm.body.get());
      cloned_arms.push_back(std::move(cloned_arm));
    }
    result = std::make_unique<CaseExprAST>(
        make_tok("case"), clone_expr(ce->scrutinee.get()),
        std::move(cloned_arms));
  } else if (auto *wh = llvm::dyn_cast<WhileExprAST>(expr)) {
    result = std::make_unique<WhileExprAST>(clone_expr(wh->condition.get()),
                                            clone_block(wh->body.get()));
  } else if (auto *tup = llvm::dyn_cast<TupleLiteralExprAST>(expr)) {
    result = std::make_unique<TupleLiteralExprAST>(
        clone_expr_vec(tup->elements));
  } else {
    sammine_util::abort("Unknown ExprAST subclass in Monomorphizer");
  }

  result->set_location(orig_loc);
  return result;
}

std::unique_ptr<FuncDefAST>
Monomorphizer::instantiate(
    FuncDefAST *generic,
    const sammine_util::MonomorphizedName &mono_name,
    const SubstitutionMap &bindings) {
  Monomorphizer m(bindings);
  auto proto = m.clone_prototype(generic->Prototype.get(), mono_name);
  auto block = m.clone_block(generic->Block.get());
  auto result = std::make_unique<FuncDefAST>(std::move(proto), std::move(block));
  result->set_location(generic->get_location());
  return result;
}

std::unique_ptr<EnumDefAST>
Monomorphizer::instantiate_enum(
    EnumDefAST *generic,
    const sammine_util::MonomorphizedName &mono_name,
    const SubstitutionMap &bindings) {
  Monomorphizer m(bindings);

  // Clone variant definitions with substituted payload types
  std::vector<EnumVariantDef> cloned_variants;
  for (auto &variant : generic->variants) {
    EnumVariantDef cloned;
    cloned.name = variant.name;
    cloned.location = variant.location;
    cloned.discriminant_value = variant.discriminant_value;
    for (auto &type_expr : variant.payload_types)
      cloned.payload_types.push_back(m.clone_type_expr(type_expr.get()));
    cloned_variants.push_back(std::move(cloned));
  }

  auto result = std::make_unique<EnumDefAST>(
      mono_name.to_qualified_name(), generic->get_location(),
      std::move(cloned_variants));
  result->is_integer_backed = generic->is_integer_backed;
  result->backing_type_name = generic->backing_type_name;
  // type_params left empty — this is a concrete instantiation
  return result;
}

std::unique_ptr<StructDefAST>
Monomorphizer::instantiate_struct(
    StructDefAST *generic,
    const sammine_util::MonomorphizedName &mono_name,
    const SubstitutionMap &bindings) {
  Monomorphizer m(bindings);

  // Clone struct members with substituted types
  std::vector<std::unique_ptr<TypedVarAST>> cloned_members;
  for (auto &member : generic->struct_members)
    cloned_members.push_back(m.clone_typed_var(member.get()));

  auto result = std::make_unique<StructDefAST>(
      mono_name.to_qualified_name(), generic->get_location(),
      std::move(cloned_members));
  // type_params left empty — this is a concrete instantiation
  return result;
}

} // namespace sammine_lang::AST
