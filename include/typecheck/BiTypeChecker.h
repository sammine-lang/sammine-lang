#pragma once

#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "typecheck/Types.h"
#include "util/LexicalContext.h"
#include <set>
#include <unordered_map>

//! \file BiTypeChecker.h
//! \brief Defines the BiTypeCheckerVisitor, consist of the flow for
//! Bi-Directional Type checking, which allows for synthesizing types,
//! validating consistency, and register types.
namespace sammine_lang {

namespace AST {

class TypingContext : public LexicalContext<Type, AST::FuncDefAST *> {};
class BiTypeCheckerVisitor : public ScopedASTVisitor,
                             public TypeCheckerVisitor {
  /// INFO: Ok let's talk about error propagation in this checker.
  /// the synthesize will error if I cannot get something out of id_map or
  /// typename_map (technically shouldn't happen)

  // We're gonna provide look up in different

public:
  // INFO: x, y, z
  LexicalStack<Type, AST::FuncDefAST *> id_to_type;

  // INFO: i64, f64 bla bla bla
  LexicalStack<Type, AST::FuncDefAST *> typename_to_type;
  TypeMapOrdering type_map_ordering;

  // Generic function support
  std::unordered_map<std::string, FuncDefAST *> generic_func_defs;
  std::vector<std::unique_ptr<FuncDefAST>> monomorphized_defs;
  std::set<std::string> instantiated_functions;

  // Unification and substitution helpers
  bool unify(const Type &pattern, const Type &concrete,
             std::unordered_map<std::string, Type> &bindings);
  Type substitute(const Type &type,
                  const std::unordered_map<std::string, Type> &bindings);
  bool contains_type_param(const Type &type, const std::string &param_name);

  virtual void enter_new_scope() override {
    id_to_type.push_context();
    typename_to_type.push_context();

    typename_to_type.registerNameT("i32", Type::I32_t());
    typename_to_type.registerNameT("i64", Type::I64_t());
    typename_to_type.registerNameT("f64", Type::F64_t());
    typename_to_type.registerNameT("bool", Type::Bool());
    typename_to_type.registerNameT("char", Type::Char());
    typename_to_type.registerNameT("unit", Type::Unit());
  }
  virtual void exit_new_scope() override {
    id_to_type.pop();
    typename_to_type.pop();
  }
  BiTypeCheckerVisitor() {
    this->enter_new_scope();
  }

  std::optional<Type> get_type_from_id(const std::string &str) const {

    const auto &id_name_top = id_to_type.top();
    if (id_name_top.queryName(str) == nameNotFound) {
      sammine_util::abort(
          fmt::format("Name '{}' not found, this should not happen", str));
    }
    return id_name_top.get_from_name(str);
  }

  std::optional<Type> get_type_from_id_parent(const std::string &str) const {

    const auto &id_name_top = *id_to_type.top().parent_scope;
    if (id_name_top.queryName(str) == nameNotFound) {
      sammine_util::abort(
          fmt::format("Name '{}' not found, this should not happen", str));
    }
    return id_name_top.get_from_name(str);
  }

  std::optional<Type> get_typename_type(const std::string &str) const {
    const auto &typename_top = typename_to_type.top();
    if (typename_top.recursiveQueryName(str) == nameNotFound) {
      return std::nullopt;
    }
    return typename_top.recursive_get_from_name(str);
  }

  /// Try to find a name in current scope + parent scopes (non-aborting)
  std::optional<Type> try_get_callee_type(const std::string &str) const {
    const auto &top = id_to_type.top();
    if (top.recursiveQueryName(str) == nameFound)
      return top.recursive_get_from_name(str);
    return std::nullopt;
  }

  // visit overrides
  virtual void visit(ProgramAST *ast) override;
  virtual void visit(VarDefAST *ast) override;
  virtual void visit(ExternAST *ast) override;
  virtual void visit(FuncDefAST *ast) override;
  virtual void visit(StructDefAST *ast) override;
  virtual void visit(EnumDefAST *ast) override;
  virtual void visit(PrototypeAST *ast) override;
  virtual void visit(CallExprAST *ast) override;
  virtual void visit(ReturnExprAST *ast) override;
  virtual void visit(BinaryExprAST *ast) override;
  virtual void visit(NumberExprAST *ast) override;
  virtual void visit(StringExprAST *ast) override;
  virtual void visit(BoolExprAST *ast) override;
  virtual void visit(CharExprAST *ast) override;
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
  virtual void visit(StructLiteralExprAST *ast) override;
  virtual void visit(FieldAccessExprAST *ast) override;
  virtual void visit(TypeClassDeclAST *ast) override;
  virtual void visit(TypeClassInstanceAST *ast) override;

  // Type class data structures
  struct TypeClassInfo {
    std::string name;
    std::string type_param;
    std::vector<PrototypeAST *> methods;
  };

  struct TypeClassInstanceInfo {
    std::string class_name;
    Type concrete_type;
    std::unordered_map<std::string, std::string> method_mangled_names;
  };

  std::unordered_map<std::string, TypeClassInfo> type_class_defs;
  std::unordered_map<std::string, TypeClassInstanceInfo> type_class_instances;
  std::unordered_map<std::string, std::string> method_to_class;

  // Enum variant constructors: variant_name → (enum_type, variant_index)
  std::unordered_map<std::string, std::pair<Type, size_t>> variant_constructors;

  // Two-pass typeclass registration (called before full type checking)
  void register_typeclass_decl(TypeClassDeclAST *ast);
  void register_typeclass_instance(TypeClassInstanceAST *ast);
  void register_builtin_op_instances();

  // Call expression synthesis helpers
  Type synthesize_typeclass_call(CallExprAST *ast);
  Type synthesize_generic_call(CallExprAST *ast);
  Type synthesize_normal_call(CallExprAST *ast);

  // Binary expression synthesis helper
  Type synthesize_binary_operator(BinaryExprAST *ast, const Type &lhs_type,
                                  const Type &rhs_type);

  // VarDef array checking helper
  bool check_array_literal_against_annotation(VarDefAST *ast,
                                              ArrayLiteralExprAST *arr_lit,
                                              const ArrayType &arr_type);

  // pre order

  virtual void preorder_walk(ProgramAST *ast) override;
  virtual void preorder_walk(VarDefAST *ast) override;
  virtual void preorder_walk(ExternAST *ast) override;
  virtual void preorder_walk(FuncDefAST *ast) override;
  virtual void preorder_walk(StructDefAST *ast) override;
  virtual void preorder_walk(EnumDefAST *ast) override;
  virtual void preorder_walk(PrototypeAST *ast) override;
  virtual void preorder_walk(CallExprAST *ast) override;
  virtual void preorder_walk(ReturnExprAST *ast) override;
  virtual void preorder_walk(BinaryExprAST *ast) override;
  virtual void preorder_walk(NumberExprAST *ast) override;
  virtual void preorder_walk(StringExprAST *ast) override;
  virtual void preorder_walk(BoolExprAST *ast) override;
  virtual void preorder_walk(CharExprAST *ast) override;
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
  virtual void preorder_walk(StructLiteralExprAST *ast) override;
  virtual void preorder_walk(FieldAccessExprAST *ast) override;
  virtual void preorder_walk(TypeClassDeclAST *ast) override;
  virtual void preorder_walk(TypeClassInstanceAST *ast) override;

  // post order
  virtual void postorder_walk(ProgramAST *ast) override;
  virtual void postorder_walk(VarDefAST *ast) override;
  virtual void postorder_walk(ExternAST *ast) override;
  virtual void postorder_walk(FuncDefAST *ast) override;
  virtual void postorder_walk(StructDefAST *ast) override;
  virtual void postorder_walk(EnumDefAST *ast) override;
  virtual void postorder_walk(PrototypeAST *ast) override;
  virtual void postorder_walk(CallExprAST *ast) override;
  virtual void postorder_walk(ReturnExprAST *ast) override;
  virtual void postorder_walk(BinaryExprAST *ast) override;
  virtual void postorder_walk(NumberExprAST *ast) override;
  virtual void postorder_walk(StringExprAST *ast) override;
  virtual void postorder_walk(BoolExprAST *ast) override;
  virtual void postorder_walk(CharExprAST *ast) override;
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
  virtual void postorder_walk(StructLiteralExprAST *ast) override;
  virtual void postorder_walk(FieldAccessExprAST *ast) override;
  virtual void postorder_walk(TypeClassDeclAST *ast) override;
  virtual void postorder_walk(TypeClassInstanceAST *ast) override;

  virtual Type synthesize(ProgramAST *ast) override;
  virtual Type synthesize(VarDefAST *ast) override;
  virtual Type synthesize(ExternAST *ast) override;
  virtual Type synthesize(FuncDefAST *ast) override;
  virtual Type synthesize(StructDefAST *ast) override;
  virtual Type synthesize(EnumDefAST *ast) override;
  virtual Type synthesize(PrototypeAST *ast) override;
  virtual Type synthesize(CallExprAST *ast) override;
  virtual Type synthesize(ReturnExprAST *ast) override;
  virtual Type synthesize(BinaryExprAST *ast) override;
  virtual Type synthesize(NumberExprAST *ast) override;
  virtual Type synthesize(UnitExprAST *ast) override;
  virtual Type synthesize(StringExprAST *ast) override;
  virtual Type synthesize(BoolExprAST *ast) override;
  virtual Type synthesize(CharExprAST *ast) override;
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
  virtual Type synthesize(StructLiteralExprAST *ast) override;
  virtual Type synthesize(FieldAccessExprAST *ast) override;
  virtual Type synthesize(TypeClassDeclAST *ast) override;
  virtual Type synthesize(TypeClassInstanceAST *ast) override;

  Type resolve_type_expr(TypeExprAST *type_expr) {
    if (!type_expr)
      return Type::NonExistent();

    if (auto *simple = dynamic_cast<SimpleTypeExprAST *>(type_expr)) {
      if (simple->name.is_unresolved()) {
        this->add_error(type_expr->location,
                        fmt::format("Module '{}' is not imported",
                                    simple->name.module));
        return Type::Poisoned();
      }
      auto mangled = simple->name.mangled();
      auto get_type_opt = this->get_typename_type(mangled);
      if (!get_type_opt.has_value()) {
        this->add_error(type_expr->location,
                        fmt::format("Type '{}' not found in the current scope.",
                                    simple->name.display()));
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
