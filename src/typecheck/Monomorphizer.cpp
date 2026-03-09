#include "typecheck/Monomorphizer.h"
#include "ast/Ast.h"
#include "lex/Token.h"
#include "util/Utilities.h"

namespace sammine_lang::AST {

static std::shared_ptr<Token> make_tok(const std::string &lexeme) {
  return std::make_shared<Token>(TokenType::TokID, lexeme,
                                 sammine_util::Location{});
}

// --- Registration ---

void Monomorphizer::register_generic_func(const std::string &mangled,
                                           FuncDefAST *def) {
  generic_func_defs_[mangled] = def;
}

void Monomorphizer::register_generic_enum(const std::string &mangled,
                                           EnumDefAST *def) {
  generic_enum_defs_[mangled] = def;
}

void Monomorphizer::register_generic_struct(const std::string &mangled,
                                             StructDefAST *def) {
  generic_struct_defs_[mangled] = def;
}

// --- Lookup ---

FuncDefAST *Monomorphizer::find_generic_func(const std::string &mangled) {
  auto it = generic_func_defs_.find(mangled);
  return it != generic_func_defs_.end() ? it->second : nullptr;
}

EnumDefAST *Monomorphizer::find_generic_enum(const std::string &mangled) {
  auto it = generic_enum_defs_.find(mangled);
  return it != generic_enum_defs_.end() ? it->second : nullptr;
}

StructDefAST *Monomorphizer::find_generic_struct(const std::string &mangled) {
  auto it = generic_struct_defs_.find(mangled);
  return it != generic_struct_defs_.end() ? it->second : nullptr;
}

// --- Instantiate ---

FuncDefAST *
Monomorphizer::try_instantiate_func(
    FuncDefAST *generic,
    const sammine_util::MonomorphizedName &mono,
    const SubstitutionMap &bindings) {
  auto mangled = mono.mangled();
  if (instantiated_functions_.contains(mangled))
    return nullptr;

  auto cloned = clone_func(generic, mono, bindings);
  auto *ptr = cloned.get();
  instantiated_functions_.insert(mangled);
  monomorphized_defs.push_back(std::move(cloned));
  return ptr;
}

EnumDefAST *
Monomorphizer::instantiate_enum(
    EnumDefAST *generic,
    const sammine_util::MonomorphizedName &mono,
    const SubstitutionMap &bindings) {
  auto cloned = clone_enum(generic, mono, bindings);
  auto *ptr = cloned.get();
  monomorphized_enum_defs.push_back(std::move(cloned));
  return ptr;
}

StructDefAST *
Monomorphizer::instantiate_struct(
    StructDefAST *generic,
    const sammine_util::MonomorphizedName &mono,
    const SubstitutionMap &bindings) {
  auto cloned = clone_struct(generic, mono, bindings);
  auto *ptr = cloned.get();
  monomorphized_struct_defs.push_back(std::move(cloned));
  return ptr;
}

// --- Clone internals ---

std::string Monomorphizer::resolve_type_name(const std::string &name) const {
  auto it = bindings_->find(name);
  if (it != bindings_->end())
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
// AllocExprAST, FreeExprAST, ArrayLiteralExprAST, RangeExprAST, IndexExprAST,
// LenExprAST, UnaryNegExprAST, StructLiteralExprAST, FieldAccessExprAST,
// CaseExprAST, WhileExprAST, TupleLiteralExprAST
std::unique_ptr<ExprAST> Monomorphizer::clone_expr(ExprAST *expr) {
  if (!expr)
    return nullptr;

  auto orig_loc = expr->get_location();
  std::unique_ptr<ExprAST> result;

  switch (expr->getKind()) {
  case NodeKind::NumberExprAST: {
    auto *num = llvm::cast<NumberExprAST>(expr);
    result = std::make_unique<NumberExprAST>(make_tok(num->number));
    break;
  }
  case NodeKind::StringExprAST: {
    auto *str = llvm::cast<StringExprAST>(expr);
    auto tok = make_tok(str->string_content);
    tok->tok_type = TokenType::TokStr;
    result = std::make_unique<StringExprAST>(tok);
    break;
  }
  case NodeKind::BoolExprAST: {
    auto *b = llvm::cast<BoolExprAST>(expr);
    result = std::make_unique<BoolExprAST>(b->b, b->get_location());
    break;
  }
  case NodeKind::CharExprAST: {
    auto *ch = llvm::cast<CharExprAST>(expr);
    result = std::make_unique<CharExprAST>(ch->value, ch->get_location());
    break;
  }
  case NodeKind::VariableExprAST: {
    auto *var = llvm::cast<VariableExprAST>(expr);
    result = std::make_unique<VariableExprAST>(make_tok(var->variableName));
    break;
  }
  case NodeKind::CallExprAST: {
    auto *call = llvm::cast<CallExprAST>(expr);
    auto r = std::make_unique<CallExprAST>(
        call->functionName, call->get_location(),
        clone_expr_vec(call->arguments));
    for (auto &ta : call->explicit_type_args)
      r->explicit_type_args.push_back(clone_type_expr(ta.get()));
    result = std::move(r);
    break;
  }
  case NodeKind::BinaryExprAST: {
    auto *bin = llvm::cast<BinaryExprAST>(expr);
    result = std::make_unique<BinaryExprAST>(
        bin->Op, clone_expr(bin->LHS.get()), clone_expr(bin->RHS.get()));
    break;
  }
  case NodeKind::ReturnExprAST: {
    auto *ret = llvm::cast<ReturnExprAST>(expr);
    if (ret->is_implicit)
      result = std::make_unique<ReturnExprAST>(clone_expr(ret->return_expr.get()));
    else
      result = std::make_unique<ReturnExprAST>(
          make_tok("return"), clone_expr(ret->return_expr.get()));
    break;
  }
  case NodeKind::VarDefAST: {
    auto *vd = llvm::cast<VarDefAST>(expr);
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
    break;
  }
  case NodeKind::IfExprAST: {
    auto *ife = llvm::cast<IfExprAST>(expr);
    result = std::make_unique<IfExprAST>(
        clone_expr(ife->bool_expr.get()), clone_block(ife->thenBlockAST.get()),
        clone_block(ife->elseBlockAST.get()));
    break;
  }
  case NodeKind::UnitExprAST: {
    auto *unit = llvm::cast<UnitExprAST>(expr);
    if (unit->is_implicit)
      result = std::make_unique<UnitExprAST>();
    else
      result = std::make_unique<UnitExprAST>(make_tok("("), make_tok(")"));
    break;
  }
  case NodeKind::DerefExprAST: {
    auto *deref = llvm::cast<DerefExprAST>(expr);
    result = std::make_unique<DerefExprAST>(make_tok("*"),
                                            clone_expr(deref->operand.get()));
    break;
  }
  case NodeKind::AddrOfExprAST: {
    auto *addr = llvm::cast<AddrOfExprAST>(expr);
    result = std::make_unique<AddrOfExprAST>(make_tok("&"),
                                             clone_expr(addr->operand.get()));
    break;
  }
  case NodeKind::AllocExprAST: {
    auto *alloc = llvm::cast<AllocExprAST>(expr);
    result = std::make_unique<AllocExprAST>(make_tok("alloc"),
                                            clone_type_expr(alloc->type_arg.get()),
                                            clone_expr(alloc->operand.get()));
    break;
  }
  case NodeKind::FreeExprAST: {
    auto *free_e = llvm::cast<FreeExprAST>(expr);
    result = std::make_unique<FreeExprAST>(make_tok("free"),
                                           clone_expr(free_e->operand.get()));
    break;
  }
  case NodeKind::ArrayLiteralExprAST: {
    auto *arr_lit = llvm::cast<ArrayLiteralExprAST>(expr);
    result = std::make_unique<ArrayLiteralExprAST>(
        clone_expr_vec(arr_lit->elements));
    break;
  }
  case NodeKind::RangeExprAST: {
    auto *range = llvm::cast<RangeExprAST>(expr);
    result = std::make_unique<RangeExprAST>(clone_expr(range->start.get()),
                                            clone_expr(range->end.get()));
    break;
  }
  case NodeKind::IndexExprAST: {
    auto *idx = llvm::cast<IndexExprAST>(expr);
    result = std::make_unique<IndexExprAST>(clone_expr(idx->array_expr.get()),
                                            clone_expr(idx->index_expr.get()));
    break;
  }
  case NodeKind::LenExprAST: {
    auto *len = llvm::cast<LenExprAST>(expr);
    result = std::make_unique<LenExprAST>(make_tok("len"),
                                          clone_expr(len->operand.get()));
    break;
  }
  case NodeKind::DimExprAST: {
    auto *dim = llvm::cast<DimExprAST>(expr);
    result = std::make_unique<DimExprAST>(make_tok("dim"),
                                          clone_expr(dim->operand.get()));
    break;
  }
  case NodeKind::UnaryNegExprAST: {
    auto *neg = llvm::cast<UnaryNegExprAST>(expr);
    result = std::make_unique<UnaryNegExprAST>(make_tok("-"),
                                               clone_expr(neg->operand.get()));
    break;
  }
  case NodeKind::StructLiteralExprAST: {
    auto *sl = llvm::cast<StructLiteralExprAST>(expr);
    auto cloned_sl = std::make_unique<StructLiteralExprAST>(
        sl->struct_name, sl->get_location(), sl->field_names,
        clone_expr_vec(sl->field_values));
    for (auto &ta : sl->explicit_type_args)
      cloned_sl->explicit_type_args.push_back(clone_type_expr(ta.get()));
    result = std::move(cloned_sl);
    break;
  }
  case NodeKind::FieldAccessExprAST: {
    auto *fa = llvm::cast<FieldAccessExprAST>(expr);
    result = std::make_unique<FieldAccessExprAST>(
        clone_expr(fa->object_expr.get()), make_tok(fa->field_name));
    break;
  }
  case NodeKind::CaseExprAST: {
    auto *ce = llvm::cast<CaseExprAST>(expr);
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
    break;
  }
  case NodeKind::WhileExprAST: {
    auto *wh = llvm::cast<WhileExprAST>(expr);
    result = std::make_unique<WhileExprAST>(clone_expr(wh->condition.get()),
                                            clone_block(wh->body.get()));
    break;
  }
  case NodeKind::TupleLiteralExprAST: {
    auto *tup = llvm::cast<TupleLiteralExprAST>(expr);
    result = std::make_unique<TupleLiteralExprAST>(
        clone_expr_vec(tup->elements));
    break;
  }
  default:
    sammine_util::abort("Unknown ExprAST subclass in Monomorphizer");
  }

  result->set_location(orig_loc);
  return result;
}

// --- Internal clone helpers ---

std::unique_ptr<FuncDefAST>
Monomorphizer::clone_func(
    FuncDefAST *generic,
    const sammine_util::MonomorphizedName &mono_name,
    const SubstitutionMap &bindings) {
  bindings_ = &bindings;
  auto proto = clone_prototype(generic->Prototype.get(), mono_name);
  auto block = clone_block(generic->Block.get());
  auto result = std::make_unique<FuncDefAST>(std::move(proto), std::move(block));
  result->set_location(generic->get_location());
  bindings_ = nullptr;
  return result;
}

std::unique_ptr<EnumDefAST>
Monomorphizer::clone_enum(
    EnumDefAST *generic,
    const sammine_util::MonomorphizedName &mono_name,
    const SubstitutionMap &bindings) {
  bindings_ = &bindings;

  // Clone variant definitions with substituted payload types
  std::vector<EnumVariantDef> cloned_variants;
  for (auto &variant : generic->variants) {
    EnumVariantDef cloned;
    cloned.name = variant.name;
    cloned.location = variant.location;
    cloned.discriminant_value = variant.discriminant_value;
    for (auto &type_expr : variant.payload_types)
      cloned.payload_types.push_back(clone_type_expr(type_expr.get()));
    cloned_variants.push_back(std::move(cloned));
  }

  auto result = std::make_unique<EnumDefAST>(
      mono_name.to_qualified_name(), generic->get_location(),
      std::move(cloned_variants));
  result->is_integer_backed = generic->is_integer_backed;
  result->backing_type_name = generic->backing_type_name;
  // type_params left empty — this is a concrete instantiation
  bindings_ = nullptr;
  return result;
}

std::unique_ptr<StructDefAST>
Monomorphizer::clone_struct(
    StructDefAST *generic,
    const sammine_util::MonomorphizedName &mono_name,
    const SubstitutionMap &bindings) {
  bindings_ = &bindings;

  // Clone struct members with substituted types
  std::vector<std::unique_ptr<TypedVarAST>> cloned_members;
  for (auto &member : generic->struct_members)
    cloned_members.push_back(clone_typed_var(member.get()));

  auto result = std::make_unique<StructDefAST>(
      mono_name.to_qualified_name(), generic->get_location(),
      std::move(cloned_members));
  // type_params left empty — this is a concrete instantiation
  bindings_ = nullptr;
  return result;
}

} // namespace sammine_lang::AST
