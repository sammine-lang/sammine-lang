#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "typecheck/Monomorphizer.h"
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
  ASTProperties &props_;
  // INFO: x, y, z
  LexicalStack<Type, AST::FuncDefAST *> id_to_type;

  // INFO: i64, f64 bla bla bla
  LexicalStack<Type, AST::FuncDefAST *> typename_to_type;
  TypeMapOrdering type_map_ordering;

  // Generic function support
  std::unordered_map<std::string, FuncDefAST *> generic_func_defs;
  std::vector<std::unique_ptr<FuncDefAST>> monomorphized_defs;
  std::set<std::string> instantiated_functions;

  // Generic enum support
  std::unordered_map<std::string, EnumDefAST *> generic_enum_defs;
  std::vector<std::unique_ptr<EnumDefAST>> monomorphized_enum_defs;
  std::set<std::string> instantiated_enums;

  // Generic struct support
  std::unordered_map<std::string, StructDefAST *> generic_struct_defs;
  std::vector<std::unique_ptr<StructDefAST>> monomorphized_struct_defs;
  std::set<std::string> instantiated_structs;

  // Unification and substitution helpers
  bool unify(const Type &pattern, const Type &concrete,
             std::unordered_map<std::string, Type> &bindings);
  Type substitute(const Type &type,
                  const std::unordered_map<std::string, Type> &bindings) const;
  bool contains_type_param(const Type &type, const std::string &param_name);

  virtual void enter_new_scope() override {
    id_to_type.push_context();
    typename_to_type.push_context();

    typename_to_type.registerNameT("i32", Type::I32_t());
    typename_to_type.registerNameT("i64", Type::I64_t());
    typename_to_type.registerNameT("u32", Type::U32_t());
    typename_to_type.registerNameT("u64", Type::U64_t());
    typename_to_type.registerNameT("f64", Type::F64_t());
    typename_to_type.registerNameT("f32", Type::F32_t());
    typename_to_type.registerNameT("bool", Type::Bool());
    typename_to_type.registerNameT("char", Type::Char());
    typename_to_type.registerNameT("unit", Type::Unit());
  }
  virtual void exit_new_scope() override {
    id_to_type.pop();
    typename_to_type.pop();
  }
  BiTypeCheckerVisitor(ASTProperties &props) : props_(props) {
    this->enter_new_scope();
    type_map_ordering.populate();
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
  virtual void visit(TypeAliasDefAST *ast) override;
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
  virtual void visit(CaseExprAST *ast) override;
  virtual void visit(WhileExprAST *ast) override;
  virtual void visit(TupleLiteralExprAST *ast) override;
  virtual void visit(TypeClassDeclAST *ast) override;
  virtual void visit(TypeClassInstanceAST *ast) override;

  // Type class data structures
  struct TypeClassInfo {
    std::string name;
    std::vector<std::string> type_params;
    std::vector<PrototypeAST *> methods;
  };

  struct TypeClassInstanceInfo {
    std::string class_name;
    std::vector<Type> concrete_types;
    std::unordered_map<std::string, sammine_util::MonomorphizedName>
        method_mangled_names;
  };

  std::unordered_map<std::string, TypeClassInfo> type_class_defs;
  std::unordered_map<std::string, TypeClassInstanceInfo> type_class_instances;
  std::unordered_map<std::string, std::string> method_to_class;

  // Enum variant constructors: variant_name → (enum_type, variant_index)
  std::unordered_map<std::string, std::pair<Type, size_t>> variant_constructors;

  // Pre-register a function signature so later definitions can reference it
  void pre_register_function(PrototypeAST *ast);

  // Two-pass typeclass registration (called before full type checking)
  void register_typeclass_decl(TypeClassDeclAST *ast);
  void register_typeclass_instance(TypeClassInstanceAST *ast);
  void register_builtin_op_instances();

  // Call expression synthesis helpers
  std::optional<Type> synthesize_typeclass_call(CallExprAST *ast);
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
  virtual void preorder_walk(TypeAliasDefAST *ast) override;
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
  virtual void preorder_walk(CaseExprAST *ast) override;
  virtual void preorder_walk(WhileExprAST *ast) override;
  virtual void preorder_walk(TupleLiteralExprAST *ast) override;
  virtual void preorder_walk(TypeClassDeclAST *ast) override;
  virtual void preorder_walk(TypeClassInstanceAST *ast) override;

  // post order
  virtual void postorder_walk(ProgramAST *ast) override;
  virtual void postorder_walk(VarDefAST *ast) override;
  virtual void postorder_walk(ExternAST *ast) override;
  virtual void postorder_walk(FuncDefAST *ast) override;
  virtual void postorder_walk(StructDefAST *ast) override;
  virtual void postorder_walk(EnumDefAST *ast) override;
  virtual void postorder_walk(TypeAliasDefAST *ast) override;
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
  virtual void postorder_walk(CaseExprAST *ast) override;
  virtual void postorder_walk(WhileExprAST *ast) override;
  virtual void postorder_walk(TupleLiteralExprAST *ast) override;
  virtual void postorder_walk(TypeClassDeclAST *ast) override;
  virtual void postorder_walk(TypeClassInstanceAST *ast) override;

  virtual Type synthesize(ProgramAST *ast) override;
  virtual Type synthesize(VarDefAST *ast) override;
  virtual Type synthesize(ExternAST *ast) override;
  virtual Type synthesize(FuncDefAST *ast) override;
  virtual Type synthesize(StructDefAST *ast) override;
  virtual Type synthesize(EnumDefAST *ast) override;
  virtual Type synthesize(TypeAliasDefAST *ast) override;
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
  virtual Type synthesize(CaseExprAST *ast) override;
  virtual Type synthesize(WhileExprAST *ast) override;
  virtual Type synthesize(TupleLiteralExprAST *ast) override;
  virtual Type synthesize(TypeClassDeclAST *ast) override;
  virtual Type synthesize(TypeClassInstanceAST *ast) override;

  Type resolve_type_expr(TypeExprAST *type_expr) {
    if (!type_expr)
      return Type::NonExistent();

    if (auto *simple = llvm::dyn_cast<SimpleTypeExprAST>(type_expr)) {
      if (simple->name.is_unresolved()) {
        this->add_error(type_expr->location,
                        fmt::format("Module '{}' is not imported",
                                    simple->name.get_module()));
        return Type::Poisoned();
      }
      auto mangled = simple->name.mangled();
      auto get_type_opt = this->get_typename_type(mangled);
      if (!get_type_opt.has_value()) {
        this->add_error(type_expr->location,
                        fmt::format("Type '{}' not found in the current scope.",
                                    simple->name.mangled()));
        return Type::Poisoned();
      }
      return get_type_opt.value();
    }

    if (auto *ptr = llvm::dyn_cast<PointerTypeExprAST>(type_expr)) {
      auto pointee = resolve_type_expr(ptr->pointee.get());
      if (pointee.is_poisoned())
        return pointee;
      auto result = Type::Pointer(pointee);
      result.linearity =
          ptr->is_linear ? Linearity::Linear : Linearity::NonLinear;
      return result;
    }

    if (auto *arr = llvm::dyn_cast<ArrayTypeExprAST>(type_expr)) {
      auto elem = resolve_type_expr(arr->element.get());
      return elem.is_poisoned() ? elem : Type::Array(elem, arr->size);
    }

    if (auto *fn = llvm::dyn_cast<FunctionTypeExprAST>(type_expr)) {
      std::vector<Type> total_types;
      for (auto &param : fn->paramTypes) {
        auto pt = resolve_type_expr(param.get());
        if (pt.is_poisoned())
          return Type::Poisoned();
        total_types.push_back(pt);
      }
      auto ret = resolve_type_expr(fn->returnType.get());
      if (ret.is_poisoned())
        return Type::Poisoned();
      total_types.push_back(ret);
      return Type::Function(std::move(total_types));
    }

    if (auto *tup = llvm::dyn_cast<TupleTypeExprAST>(type_expr)) {
      std::vector<Type> elem_types;
      for (auto &et : tup->element_types) {
        auto resolved = resolve_type_expr(et.get());
        if (resolved.is_poisoned())
          return Type::Poisoned();
        elem_types.push_back(resolved);
      }
      return Type::Tuple(std::move(elem_types));
    }

    if (auto *gen = llvm::dyn_cast<GenericTypeExprAST>(type_expr)) {
      auto base_mangled = gen->base_name.mangled();

      // Determine if this is a generic enum or generic struct
      auto enum_it = generic_enum_defs.find(base_mangled);
      auto struct_it = generic_struct_defs.find(base_mangled);
      bool is_enum = (enum_it != generic_enum_defs.end());
      bool is_struct = (struct_it != generic_struct_defs.end());

      if (!is_enum && !is_struct) {
        this->add_error(type_expr->location,
                        fmt::format("'{}' is not a generic type",
                                    gen->base_name.mangled()));
        return Type::Poisoned();
      }

      size_t expected_params = is_enum ? enum_it->second->type_params.size()
                                       : struct_it->second->type_params.size();
      const auto &param_names = is_enum ? enum_it->second->type_params
                                        : struct_it->second->type_params;

      if (gen->type_args.size() != expected_params) {
        this->add_error(
            type_expr->location,
            fmt::format("Generic type '{}' expects {} type argument(s), got {}",
                        gen->base_name.mangled(), expected_params,
                        gen->type_args.size()));
        return Type::Poisoned();
      }

      // Resolve type arguments
      Monomorphizer::SubstitutionMap bindings;
      std::string type_args = "<";
      bool has_unresolved_type_param = false;
      for (size_t i = 0; i < gen->type_args.size(); i++) {
        auto resolved = resolve_type_expr(gen->type_args[i].get());
        if (resolved.is_poisoned())
          return Type::Poisoned();
        if (resolved.type_kind == TypeKind::TypeParam)
          has_unresolved_type_param = true;
        bindings[param_names[i]] = resolved;
        if (i > 0)
          type_args += ", ";
        type_args += resolved.to_string();
      }
      type_args += ">";
      auto mono =
          sammine_util::MonomorphizedName::generic(gen->base_name, type_args);
      auto mangled = mono.mangled();

      // If type args contain unresolved type params (e.g. Vec<T> inside
      // a generic function prototype), we can't instantiate yet — build a
      // placeholder StructType with TypeParam fields so substitute() can
      // replace them when concrete bindings are known.
      if (has_unresolved_type_param) {
        auto existing = this->get_typename_type(mangled);
        if (existing.has_value())
          return existing.value();

        if (is_struct) {
          auto *sdef = struct_it->second;
          std::vector<std::string> fnames;
          std::vector<Type> ftypes;
          for (auto &[pname, ptype] : bindings)
            typename_to_type.registerNameT(pname, ptype);
          for (auto &member : sdef->struct_members) {
            fnames.push_back(member->name);
            ftypes.push_back(member->type_expr
                                 ? resolve_type_expr(member->type_expr.get())
                                 : Type::Poisoned());
          }
          auto placeholder =
              Type::Struct(sammine_util::QualifiedName::from_parts({mangled}),
                           std::move(fnames), std::move(ftypes));
          typename_to_type.registerNameT(mangled, placeholder);
          return placeholder;
        }
        return Type::Poisoned();
      }

      if (is_enum) {
        // Already instantiated?
        if (instantiated_enums.contains(mangled)) {
          auto existing = this->get_typename_type(mangled);
          if (existing.has_value())
            return existing.value();
        }

        // Instantiate the generic enum
        auto cloned =
            Monomorphizer::instantiate_enum(enum_it->second, mono, bindings);
        cloned->accept_vis(this);
        instantiated_enums.insert(mangled);
        monomorphized_enum_defs.push_back(std::move(cloned));
      } else {
        // Already instantiated?
        if (instantiated_structs.contains(mangled)) {
          auto existing = this->get_typename_type(mangled);
          if (existing.has_value())
            return existing.value();
        }

        // Instantiate the generic struct
        auto cloned = Monomorphizer::instantiate_struct(struct_it->second, mono,
                                                        bindings);
        cloned->accept_vis(this);
        instantiated_structs.insert(mangled);
        monomorphized_struct_defs.push_back(std::move(cloned));
      }

      auto result = this->get_typename_type(mangled);
      return result.has_value() ? result.value() : Type::Poisoned();
    }

    return Type::NonExistent();
  }
};
// --- Numeric literal type inference helpers (shared across .cpp files) ---

/// Default a polymorphic numeric type to its concrete default:
/// Integer → I32_t, Flt → F64_t. Non-polymorphic types pass through unchanged.
inline Type default_polymorphic_type(const Type &t) {
  if (t.type_kind == TypeKind::Integer)
    return Type::I32_t();
  if (t.type_kind == TypeKind::Flt)
    return Type::F64_t();
  return t;
}

/// Recursively resolve polymorphic literal types in an expression tree
/// to a concrete target type. Walks through UnaryNeg, BinaryExpr, IfExpr,
/// and BlockAST to reach all leaf literals.
inline void resolve_literal_type(ExprAST *expr, const Type &target) {
  if (!expr || !expr->get_type().is_polymorphic_numeric())
    return;

  expr->set_type(target);

  if (auto *unary = llvm::dyn_cast<UnaryNegExprAST>(expr)) {
    resolve_literal_type(unary->operand.get(), target);
  } else if (auto *binary = llvm::dyn_cast<BinaryExprAST>(expr)) {
    resolve_literal_type(binary->LHS.get(), target);
    resolve_literal_type(binary->RHS.get(), target);
  } else if (auto *if_expr = llvm::dyn_cast<IfExprAST>(expr)) {
    if (if_expr->thenBlockAST && !if_expr->thenBlockAST->Statements.empty()) {
      auto *last_then = if_expr->thenBlockAST->Statements.back().get();
      resolve_literal_type(last_then, target);
      if_expr->thenBlockAST->set_type(target);
    }
    if (if_expr->elseBlockAST && !if_expr->elseBlockAST->Statements.empty()) {
      auto *last_else = if_expr->elseBlockAST->Statements.back().get();
      resolve_literal_type(last_else, target);
      if_expr->elseBlockAST->set_type(target);
    }
  }
  // For all other expression types (NumberExprAST, CallExprAST, IndexExprAST,
  // etc.), the type is already set above — no children to recurse into.
}

} // namespace AST
} // namespace sammine_lang
