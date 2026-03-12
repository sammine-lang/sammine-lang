#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "typecheck/Monomorphizer.h"
#include "typecheck/Types.h"
#include "util/LexicalContext.h"
#include <unordered_map>
#include <unordered_set>

//! \file BiTypeChecker.h
//! \brief Defines the BiTypeCheckerVisitor, consist of the flow for
//! Bi-Directional Type checking, which allows for synthesizing types,
//! validating consistency, and register types.
namespace sammine_lang {

namespace AST {

class BiTypeCheckerVisitor : public ScopedASTVisitor,
                             public TypeCheckerVisitor {
  /// INFO: Ok let's talk about error propagation in this checker.
  /// the synthesize will error if I cannot get something out of id_map or
  /// typename_map (technically shouldn't happen)

  // We're gonna provide look up in different

public:
  ASTProperties &props_;
  // INFO: x, y, z
  LexicalStack<Type> id_to_type;

  // INFO: i64, f64 bla bla bla
  LexicalStack<Type> typename_to_type;
  TypeMapOrdering type_map_ordering;

  // Monomorphization state: generic registration, dedup, cloning, output
  Monomorphizer monomorphizer;

  virtual void enter_new_scope() override {
    push_ast_context();
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
    pop_ast_context();
  }
  BiTypeCheckerVisitor(ASTProperties &props) : props_(props) {
    this->enter_new_scope();
    type_map_ordering.populate();
  }

  std::optional<Type> get_typename_type(const std::string &str) const {
    return typename_to_type.recursive_try_get(str);
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
  virtual void visit(ReturnStmtAST *ast) override;
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
  virtual void visit(RangeExprAST *ast) override;
  virtual void visit(IndexExprAST *ast) override;
  virtual void visit(LenExprAST *ast) override;
  virtual void visit(DimExprAST *ast) override;
  virtual void visit(UnaryNegExprAST *ast) override;
  virtual void visit(StructLiteralExprAST *ast) override;
  virtual void visit(FieldAccessExprAST *ast) override;
  virtual void visit(CaseExprAST *ast) override;
  virtual void visit(WhileExprAST *ast) override;
  virtual void visit(TupleLiteralExprAST *ast) override;
  virtual void visit(TypeClassDeclAST *ast) override;
  virtual void visit(TypeClassInstanceAST *ast) override;
  virtual void visit(KernelDefAST *ast) override;

  // Type class data structures
  std::unordered_map<std::string, TypeClassDeclAST *> type_class_defs;
  std::unordered_set<MonomorphizedKey, MonomorphizedKeyHash>
      type_class_instances;
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
  std::optional<Type> synthesize_generic_call(CallExprAST *ast);
  std::optional<Type> synthesize_normal_call(CallExprAST *ast);

  /// Resolve explicit type args against type params, producing bindings and
  /// the resolved types in order. Returns nullopt if any type arg resolves
  /// to Poisoned. Caller must check size equality beforehand.
  std::optional<std::pair<TypeBindings, std::vector<Type>>>
  resolve_explicit_type_args(
      const std::vector<std::unique_ptr<TypeExprAST>> &explicit_type_args,
      const std::vector<std::string> &type_params);

  // Binary expression synthesis helper
  Type synthesize_binary_operator(BinaryExprAST *ast, const Type &lhs_type,
                                  const Type &rhs_type);


  // Kernel intrinsic synthesis helpers
  Type synthesize_kernel_map(KernelDefAST *kd, KernelMapExprAST *map_expr);
  Type synthesize_kernel_reduce(KernelDefAST *kd, KernelReduceExprAST *reduce_expr);

  // pre order


  // post order

  virtual Type synthesize(ProgramAST *ast) override;
  virtual Type synthesize(VarDefAST *ast) override;
  virtual Type synthesize(ExternAST *ast) override;
  virtual Type synthesize(FuncDefAST *ast) override;
  virtual Type synthesize(StructDefAST *ast) override;
  virtual Type synthesize(EnumDefAST *ast) override;
  virtual Type synthesize(TypeAliasDefAST *ast) override;
  virtual Type synthesize(PrototypeAST *ast) override;
  virtual Type synthesize(CallExprAST *ast) override;
  virtual Type synthesize(ReturnStmtAST *ast) override;
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
  virtual Type synthesize(RangeExprAST *ast) override;
  virtual Type synthesize(IndexExprAST *ast) override;
  virtual Type synthesize(LenExprAST *ast) override;
  virtual Type synthesize(DimExprAST *ast) override;
  virtual Type synthesize(UnaryNegExprAST *ast) override;
  virtual Type synthesize(StructLiteralExprAST *ast) override;
  virtual Type synthesize(FieldAccessExprAST *ast) override;
  virtual Type synthesize(CaseExprAST *ast) override;
  virtual Type synthesize(WhileExprAST *ast) override;
  virtual Type synthesize(TupleLiteralExprAST *ast) override;
  virtual Type synthesize(TypeClassDeclAST *ast) override;
  virtual Type synthesize(TypeClassInstanceAST *ast) override;
  virtual Type synthesize(KernelDefAST *ast) override;

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
      auto *enum_def = monomorphizer.generic_enums.find(base_mangled);
      auto *struct_def = monomorphizer.generic_structs.find(base_mangled);
      bool is_enum = (enum_def != nullptr);
      bool is_struct = (struct_def != nullptr);

      if (!is_enum && !is_struct) {
        this->add_error(type_expr->location,
                        fmt::format("'{}' is not a generic type",
                                    gen->base_name.mangled()));
        return Type::Poisoned();
      }

      size_t expected_params = is_enum ? enum_def->type_params.size()
                                       : struct_def->type_params.size();
      const auto &param_names = is_enum ? enum_def->type_params
                                        : struct_def->type_params;

      if (gen->type_args.size() != expected_params) {
        this->add_error(
            type_expr->location,
            fmt::format("Generic type '{}' expects {} type argument(s), got {}",
                        gen->base_name.mangled(), expected_params,
                        gen->type_args.size()));
        return Type::Poisoned();
      }

      // Resolve type arguments
      TypeBindings bindings;
      std::vector<Type> resolved_type_args;
      bool has_unresolved_type_param = false;
      for (size_t i = 0; i < gen->type_args.size(); i++) {
        auto resolved = resolve_type_expr(gen->type_args[i].get());
        if (resolved.is_poisoned())
          return Type::Poisoned();
        if (resolved.type_kind == TypeKind::TypeParam)
          has_unresolved_type_param = true;
        bindings[param_names[i]] = resolved;
        resolved_type_args.push_back(resolved);
      }
      MonomorphizedKey key{gen->base_name.mangled(), resolved_type_args};
      auto mono = key.to_generic_name(gen->base_name);
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
          auto *sdef = struct_def;
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

      // Check if already instantiated (registered at root scope)
      {
        auto existing = this->get_typename_type(mangled);
        if (existing.has_value())
          return existing.value();
      }

      // Not yet instantiated — clone + type-check
      if (is_enum)
        monomorphizer.instantiate_enum(enum_def, key, bindings)
            ->accept_vis(this);
      else
        monomorphizer.instantiate_struct(struct_def, key, bindings)
            ->accept_vis(this);

      auto result = this->get_typename_type(mangled);
      return result.has_value() ? result.value() : Type::Poisoned();
    }

    return Type::NonExistent();
  }
};
// --- Numeric literal type inference helpers (shared across .cpp files) ---

/// Default a polymorphic numeric type to its concrete default:
/// Integer → I32_t, Flt → F64_t. Recurses into Array and Tuple.
/// Non-polymorphic types pass through unchanged.
inline Type default_polymorphic_type(const Type &t) {
  if (t.type_kind == TypeKind::Integer)
    return Type::I32_t();
  if (t.type_kind == TypeKind::Flt)
    return Type::F64_t();
  if (t.type_kind == TypeKind::Array) {
    auto &arr = std::get<ArrayType>(t.type_data);
    auto elem = default_polymorphic_type(arr.get_element());
    if (elem != arr.get_element())
      return Type::Array(elem, arr.get_size());
  }
  if (t.type_kind == TypeKind::Tuple) {
    auto &tup = std::get<TupleType>(t.type_data);
    std::vector<Type> elems;
    bool changed = false;
    for (size_t i = 0; i < tup.size(); i++) {
      auto elem = default_polymorphic_type(tup.get_element(i));
      if (elem != tup.get_element(i))
        changed = true;
      elems.push_back(elem);
    }
    if (changed)
      return Type::Tuple(std::move(elems));
  }
  return t;
}

/// Recursively resolve polymorphic literal types in an expression tree
/// to a concrete target type. Walks through UnaryNeg, BinaryExpr, IfExpr,
/// ArrayLiteral, and TupleLiteral to reach all leaf literals.
inline void resolve_literal_type(ExprAST *expr, const Type &target) {
  if (!expr)
    return;

  // Compound types: recurse into elements
  if (auto *arr = llvm::dyn_cast<ArrayLiteralExprAST>(expr)) {
    if (target.type_kind == TypeKind::Array) {
      auto &arr_type = std::get<ArrayType>(target.type_data);
      for (auto &elem : arr->elements)
        resolve_literal_type(elem.get(), arr_type.get_element());
      expr->set_type(target);
    }
    return;
  }
  if (auto *tup = llvm::dyn_cast<TupleLiteralExprAST>(expr)) {
    if (target.type_kind == TypeKind::Tuple) {
      auto &tup_type = std::get<TupleType>(target.type_data);
      for (size_t i = 0; i < tup->elements.size() && i < tup_type.size(); i++)
        resolve_literal_type(tup->elements[i].get(), tup_type.get_element(i));
      expr->set_type(target);
    }
    return;
  }

  // Scalar polymorphic literals
  if (!expr->get_type().is_polymorphic_numeric())
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
}

} // namespace AST
} // namespace sammine_lang
