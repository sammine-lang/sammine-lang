#include "ast/AstWalkers.h"
#include "ast/Ast.h"

namespace sammine_lang::AST {

void collect_call_names(const ExprAST *expr, std::set<std::string> &names) {
  if (!expr)
    return;
  if (auto *call = llvm::dyn_cast<CallExprAST>(expr)) {
    names.insert(call->functionName.get_name());
    for (auto &arg : call->arguments)
      collect_call_names(arg.get(), names);
  } else if (auto *bin = llvm::dyn_cast<BinaryExprAST>(expr)) {
    collect_call_names(bin->LHS.get(), names);
    collect_call_names(bin->RHS.get(), names);
  } else if (auto *ret = llvm::dyn_cast<ReturnExprAST>(expr)) {
    collect_call_names(ret->return_expr.get(), names);
  } else if (auto *vardef = llvm::dyn_cast<VarDefAST>(expr)) {
    collect_call_names(vardef->Expression.get(), names);
  } else if (auto *ifexpr = llvm::dyn_cast<IfExprAST>(expr)) {
    collect_call_names(ifexpr->bool_expr.get(), names);
    if (ifexpr->thenBlockAST)
      for (auto &s : ifexpr->thenBlockAST->Statements)
        collect_call_names(s.get(), names);
    if (ifexpr->elseBlockAST)
      for (auto &s : ifexpr->elseBlockAST->Statements)
        collect_call_names(s.get(), names);
  } else if (auto *wh = llvm::dyn_cast<WhileExprAST>(expr)) {
    collect_call_names(wh->condition.get(), names);
    if (wh->body)
      for (auto &s : wh->body->Statements)
        collect_call_names(s.get(), names);
  } else if (auto *cs = llvm::dyn_cast<CaseExprAST>(expr)) {
    collect_call_names(cs->scrutinee.get(), names);
    for (auto &arm : cs->arms)
      if (arm.body)
        for (auto &s : arm.body->Statements)
          collect_call_names(s.get(), names);
  } else if (auto *idx = llvm::dyn_cast<IndexExprAST>(expr)) {
    collect_call_names(idx->array_expr.get(), names);
    collect_call_names(idx->index_expr.get(), names);
  } else if (auto *arr = llvm::dyn_cast<ArrayLiteralExprAST>(expr)) {
    for (auto &e : arr->elements)
      collect_call_names(e.get(), names);
  } else if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(expr)) {
    for (auto &v : sl->field_values)
      collect_call_names(v.get(), names);
  } else if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(expr)) {
    collect_call_names(fa->object_expr.get(), names);
  } else if (auto *deref = llvm::dyn_cast<DerefExprAST>(expr)) {
    collect_call_names(deref->operand.get(), names);
  } else if (auto *addr = llvm::dyn_cast<AddrOfExprAST>(expr)) {
    collect_call_names(addr->operand.get(), names);
  } else if (auto *alloc = llvm::dyn_cast<AllocExprAST>(expr)) {
    collect_call_names(alloc->operand.get(), names);
  } else if (auto *fr = llvm::dyn_cast<FreeExprAST>(expr)) {
    collect_call_names(fr->operand.get(), names);
  } else if (auto *ln = llvm::dyn_cast<LenExprAST>(expr)) {
    collect_call_names(ln->operand.get(), names);
  } else if (auto *neg = llvm::dyn_cast<UnaryNegExprAST>(expr)) {
    collect_call_names(neg->operand.get(), names);
  } else if (auto *tup = llvm::dyn_cast<TupleLiteralExprAST>(expr)) {
    for (auto &e : tup->elements)
      collect_call_names(e.get(), names);
  }
  // NumberExprAST, StringExprAST, BoolExprAST, CharExprAST,
  // VariableExprAST, UnitExprAST — no children to recurse into
}

void collect_type_names(const TypeExprAST *expr,
                        std::set<std::string> &names) {
  if (!expr)
    return;
  if (auto *simple = llvm::dyn_cast<SimpleTypeExprAST>(expr)) {
    names.insert(simple->name.get_name());
  } else if (auto *ptr = llvm::dyn_cast<PointerTypeExprAST>(expr)) {
    collect_type_names(ptr->pointee.get(), names);
  } else if (auto *arr = llvm::dyn_cast<ArrayTypeExprAST>(expr)) {
    collect_type_names(arr->element.get(), names);
  } else if (auto *gen = llvm::dyn_cast<GenericTypeExprAST>(expr)) {
    names.insert(gen->base_name.get_name());
    for (auto &arg : gen->type_args)
      collect_type_names(arg.get(), names);
  } else if (auto *func = llvm::dyn_cast<FunctionTypeExprAST>(expr)) {
    for (auto &p : func->paramTypes)
      collect_type_names(p.get(), names);
    collect_type_names(func->returnType.get(), names);
  } else if (auto *tup = llvm::dyn_cast<TupleTypeExprAST>(expr)) {
    for (auto &e : tup->element_types)
      collect_type_names(e.get(), names);
  }
}

void collect_expr_type_names(const ExprAST *expr,
                             std::set<std::string> &names) {
  if (!expr)
    return;
  if (auto *vardef = llvm::dyn_cast<VarDefAST>(expr)) {
    if (vardef->TypedVar && vardef->TypedVar->type_expr)
      collect_type_names(vardef->TypedVar->type_expr.get(), names);
    collect_expr_type_names(vardef->Expression.get(), names);
  } else if (auto *alloc = llvm::dyn_cast<AllocExprAST>(expr)) {
    collect_type_names(alloc->type_arg.get(), names);
    collect_expr_type_names(alloc->operand.get(), names);
  } else if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(expr)) {
    names.insert(sl->struct_name.get_name());
    for (auto &v : sl->field_values)
      collect_expr_type_names(v.get(), names);
  } else if (auto *call = llvm::dyn_cast<CallExprAST>(expr)) {
    for (auto &ta : call->explicit_type_args)
      collect_type_names(ta.get(), names);
    for (auto &arg : call->arguments)
      collect_expr_type_names(arg.get(), names);
  } else if (auto *bin = llvm::dyn_cast<BinaryExprAST>(expr)) {
    collect_expr_type_names(bin->LHS.get(), names);
    collect_expr_type_names(bin->RHS.get(), names);
  } else if (auto *ret = llvm::dyn_cast<ReturnExprAST>(expr)) {
    collect_expr_type_names(ret->return_expr.get(), names);
  } else if (auto *ifexpr = llvm::dyn_cast<IfExprAST>(expr)) {
    collect_expr_type_names(ifexpr->bool_expr.get(), names);
    if (ifexpr->thenBlockAST)
      for (auto &s : ifexpr->thenBlockAST->Statements)
        collect_expr_type_names(s.get(), names);
    if (ifexpr->elseBlockAST)
      for (auto &s : ifexpr->elseBlockAST->Statements)
        collect_expr_type_names(s.get(), names);
  } else if (auto *wh = llvm::dyn_cast<WhileExprAST>(expr)) {
    collect_expr_type_names(wh->condition.get(), names);
    if (wh->body)
      for (auto &s : wh->body->Statements)
        collect_expr_type_names(s.get(), names);
  } else if (auto *cs = llvm::dyn_cast<CaseExprAST>(expr)) {
    collect_expr_type_names(cs->scrutinee.get(), names);
    for (auto &arm : cs->arms)
      if (arm.body)
        for (auto &s : arm.body->Statements)
          collect_expr_type_names(s.get(), names);
  } else if (auto *idx = llvm::dyn_cast<IndexExprAST>(expr)) {
    collect_expr_type_names(idx->array_expr.get(), names);
    collect_expr_type_names(idx->index_expr.get(), names);
  } else if (auto *arr = llvm::dyn_cast<ArrayLiteralExprAST>(expr)) {
    for (auto &e : arr->elements)
      collect_expr_type_names(e.get(), names);
  } else if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(expr)) {
    collect_expr_type_names(fa->object_expr.get(), names);
  } else if (auto *deref = llvm::dyn_cast<DerefExprAST>(expr)) {
    collect_expr_type_names(deref->operand.get(), names);
  } else if (auto *addr = llvm::dyn_cast<AddrOfExprAST>(expr)) {
    collect_expr_type_names(addr->operand.get(), names);
  } else if (auto *fr = llvm::dyn_cast<FreeExprAST>(expr)) {
    collect_expr_type_names(fr->operand.get(), names);
  } else if (auto *ln = llvm::dyn_cast<LenExprAST>(expr)) {
    collect_expr_type_names(ln->operand.get(), names);
  } else if (auto *neg = llvm::dyn_cast<UnaryNegExprAST>(expr)) {
    collect_expr_type_names(neg->operand.get(), names);
  } else if (auto *tup = llvm::dyn_cast<TupleLiteralExprAST>(expr)) {
    for (auto &e : tup->elements)
      collect_expr_type_names(e.get(), names);
  }
  // NumberExprAST, StringExprAST, BoolExprAST, CharExprAST,
  // VariableExprAST, UnitExprAST — no type references to collect
}

} // namespace sammine_lang::AST
