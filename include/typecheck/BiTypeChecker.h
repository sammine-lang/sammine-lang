#pragma once

#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "typecheck/Types.h"
#include "util/LexicalContext.h"

//! \file BiTypeChecker.h
//! \brief Defines the BiTypeCheckerVisitor, consist of the flow for
//! Bi-Directional Type checking, which allows for synthesizing types,
//! validating consistency, and register types.
namespace sammine_lang {

namespace AST {

class TypingContext : public LexicalContext<Type, AST::FuncDefAST*> {};
class BiTypeCheckerVisitor : public ScopedASTVisitor,
                             public TypeCheckerVisitor {
  /// INFO: Ok let's talk about error propagation in this checker.
  /// the synthesize will error if I cannot get something out of id_map or
  /// typename_map (technically shouldn't happen)

  // We're gonna provide look up in different

public:
  // INFO: x, y, z
  LexicalStack<Type, AST::FuncDefAST*> id_to_type;

  // INFO: i64, f64 bla bla bla
  LexicalStack<Type, AST::FuncDefAST*> typename_to_type;
  TypeMapOrdering type_map_ordering;
  const std::set<std::string> &pre_func;
  virtual void enter_new_scope() override {
    id_to_type.push_context();
    typename_to_type.push_context();

    typename_to_type.registerNameT("i32", Type::I32_t());
    typename_to_type.registerNameT("i64", Type::I64_t());
    typename_to_type.registerNameT("f64", Type::F64_t());
    typename_to_type.registerNameT("bool", Type::Bool());
    typename_to_type.registerNameT("unit", Type::Unit());
  }
  virtual void exit_new_scope() override {
    id_to_type.pop();
    typename_to_type.pop();
  }
  BiTypeCheckerVisitor(const std::set<std::string> &pre_func) : pre_func(pre_func) { this->enter_new_scope(); }

  std::optional<Type> get_type_from_id(const std::string &str) {

    auto &id_name_top = id_to_type.top();
    if (id_name_top.queryName(str) == nameNotFound) {
      this->abort(
          fmt::format("Name '{}' not found, this should not happen", str));
    }
    return id_name_top.get_from_name(str);
  }

  std::optional<Type> get_type_from_id_parent(const std::string &str) {

    auto &id_name_top = *id_to_type.top().parent_scope;
    if (id_name_top.queryName(str) == nameNotFound) {
      this->abort(
          fmt::format("Name '{}' not found, this should not happen", str));
    }
    return id_name_top.get_from_name(str);
  }

  std::optional<Type> get_typename_type(const std::string &str) {
    auto &typename_top = typename_to_type.top();
    if (typename_top.queryName(str) == nameNotFound) {
      return std::nullopt;
    }
    return typename_top.get_from_name(str);
  }

  // visit overrides
  virtual void visit(ProgramAST *ast) override;
  virtual void visit(VarDefAST *ast) override;
  virtual void visit(ExternAST *ast) override;
  virtual void visit(FuncDefAST *ast) override;
  virtual void visit(RecordDefAST *ast) override;
  virtual void visit(PrototypeAST *ast) override;
  virtual void visit(CallExprAST *ast) override;
  virtual void visit(ReturnExprAST *ast) override;
  virtual void visit(BinaryExprAST *ast) override;
  virtual void visit(NumberExprAST *ast) override;
  virtual void visit(StringExprAST *ast) override;
  virtual void visit(BoolExprAST *ast) override;
  virtual void visit(UnitExprAST *ast) override;
  virtual void visit(VariableExprAST *ast) override;
  virtual void visit(BlockAST *ast) override;
  virtual void visit(IfExprAST *ast) override;
  virtual void visit(TypedVarAST *ast) override;
  virtual void visit(DerefExprAST *ast) override;
  virtual void visit(AddrOfExprAST *ast) override;
  virtual void visit(AllocExprAST *ast) override;
  virtual void visit(FreeExprAST *ast) override;
  virtual void visit(ArrayLiteralExprAST *ast) override;
  virtual void visit(IndexExprAST *ast) override;
  virtual void visit(LenExprAST *ast) override;
  virtual void visit(UnaryNegExprAST *ast) override;

  // pre order

  virtual void preorder_walk(ProgramAST *ast) override;
  virtual void preorder_walk(VarDefAST *ast) override;
  virtual void preorder_walk(ExternAST *ast) override;
  virtual void preorder_walk(FuncDefAST *ast) override;
  virtual void preorder_walk(RecordDefAST *ast) override;
  virtual void preorder_walk(PrototypeAST *ast) override;
  virtual void preorder_walk(CallExprAST *ast) override;
  virtual void preorder_walk(ReturnExprAST *ast) override;
  virtual void preorder_walk(BinaryExprAST *ast) override;
  virtual void preorder_walk(NumberExprAST *ast) override;
  virtual void preorder_walk(StringExprAST *ast) override;
  virtual void preorder_walk(BoolExprAST *ast) override;
  virtual void preorder_walk(UnitExprAST *ast) override;
  virtual void preorder_walk(VariableExprAST *ast) override;
  virtual void preorder_walk(BlockAST *ast) override;
  virtual void preorder_walk(IfExprAST *ast) override;
  virtual void preorder_walk(TypedVarAST *ast) override;
  virtual void preorder_walk(DerefExprAST *ast) override;
  virtual void preorder_walk(AddrOfExprAST *ast) override;
  virtual void preorder_walk(AllocExprAST *ast) override;
  virtual void preorder_walk(FreeExprAST *ast) override;
  virtual void preorder_walk(ArrayLiteralExprAST *ast) override;
  virtual void preorder_walk(IndexExprAST *ast) override;
  virtual void preorder_walk(LenExprAST *ast) override;
  virtual void preorder_walk(UnaryNegExprAST *ast) override;

  // post order
  virtual void postorder_walk(ProgramAST *ast) override;
  virtual void postorder_walk(VarDefAST *ast) override;
  virtual void postorder_walk(ExternAST *ast) override;
  virtual void postorder_walk(FuncDefAST *ast) override;
  virtual void postorder_walk(RecordDefAST *ast) override;
  virtual void postorder_walk(PrototypeAST *ast) override;
  virtual void postorder_walk(CallExprAST *ast) override;
  virtual void postorder_walk(ReturnExprAST *ast) override;
  virtual void postorder_walk(BinaryExprAST *ast) override;
  virtual void postorder_walk(NumberExprAST *ast) override;
  virtual void postorder_walk(StringExprAST *ast) override;
  virtual void postorder_walk(BoolExprAST *ast) override;
  virtual void postorder_walk(UnitExprAST *ast) override;
  virtual void postorder_walk(VariableExprAST *ast) override;
  virtual void postorder_walk(BlockAST *ast) override;
  virtual void postorder_walk(IfExprAST *ast) override;
  virtual void postorder_walk(TypedVarAST *ast) override;
  virtual void postorder_walk(DerefExprAST *ast) override;
  virtual void postorder_walk(AddrOfExprAST *ast) override;
  virtual void postorder_walk(AllocExprAST *ast) override;
  virtual void postorder_walk(FreeExprAST *ast) override;
  virtual void postorder_walk(ArrayLiteralExprAST *ast) override;
  virtual void postorder_walk(IndexExprAST *ast) override;
  virtual void postorder_walk(LenExprAST *ast) override;
  virtual void postorder_walk(UnaryNegExprAST *ast) override;

  virtual Type synthesize(ProgramAST *ast) override;
  virtual Type synthesize(VarDefAST *ast) override;
  virtual Type synthesize(ExternAST *ast) override;
  virtual Type synthesize(FuncDefAST *ast) override;
  virtual Type synthesize(RecordDefAST *ast) override;
  virtual Type synthesize(PrototypeAST *ast) override;
  virtual Type synthesize(CallExprAST *ast) override;
  virtual Type synthesize(ReturnExprAST *ast) override;
  virtual Type synthesize(BinaryExprAST *ast) override;
  virtual Type synthesize(NumberExprAST *ast) override;
  virtual Type synthesize(UnitExprAST *ast) override;
  virtual Type synthesize(StringExprAST *ast) override;
  virtual Type synthesize(BoolExprAST *ast) override;
  virtual Type synthesize(VariableExprAST *ast) override;
  virtual Type synthesize(BlockAST *ast) override;
  virtual Type synthesize(IfExprAST *ast) override;
  virtual Type synthesize(TypedVarAST *ast) override;
  virtual Type synthesize(DerefExprAST *ast) override;
  virtual Type synthesize(AddrOfExprAST *ast) override;
  virtual Type synthesize(AllocExprAST *ast) override;
  virtual Type synthesize(FreeExprAST *ast) override;
  virtual Type synthesize(ArrayLiteralExprAST *ast) override;
  virtual Type synthesize(IndexExprAST *ast) override;
  virtual Type synthesize(LenExprAST *ast) override;
  virtual Type synthesize(UnaryNegExprAST *ast) override;

  Type resolve_type_expr(TypeExprAST *type_expr) {
    if (!type_expr)
      return Type::NonExistent();

    if (auto *simple = dynamic_cast<SimpleTypeExprAST *>(type_expr)) {
      auto get_type_opt = this->get_typename_type(simple->name);
      if (!get_type_opt.has_value()) {
        this->add_error(type_expr->location,
                        fmt::format("Type '{}' not found in the current scope.",
                                    simple->name));
        return Type::Poisoned();
      }
      return get_type_opt.value();
    }

    if (auto *ptr = dynamic_cast<PointerTypeExprAST *>(type_expr)) {
      auto pointee = resolve_type_expr(ptr->pointee.get());
      if (pointee.type_kind == TypeKind::Poisoned)
        return Type::Poisoned();
      return Type::Pointer(pointee);
    }

    if (auto *arr = dynamic_cast<ArrayTypeExprAST *>(type_expr)) {
      auto elem = resolve_type_expr(arr->element.get());
      if (elem.type_kind == TypeKind::Poisoned)
        return Type::Poisoned();
      return Type::Array(elem, arr->size);
    }

    if (auto *fn = dynamic_cast<FunctionTypeExprAST *>(type_expr)) {
      std::vector<Type> total_types;
      for (auto &param : fn->paramTypes) {
        auto pt = resolve_type_expr(param.get());
        if (pt.type_kind == TypeKind::Poisoned)
          return Type::Poisoned();
        total_types.push_back(pt);
      }
      auto ret = resolve_type_expr(fn->returnType.get());
      if (ret.type_kind == TypeKind::Poisoned)
        return Type::Poisoned();
      total_types.push_back(ret);
      return Type::Function(std::move(total_types));
    }

    return Type::NonExistent();
  }
};
} // namespace AST
} // namespace sammine_lang
