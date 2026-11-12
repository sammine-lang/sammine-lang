#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "typecheck/Monomorphizer.h"
#include "typecheck/Types.h"
#include "util/LexicalContext.h"
#include <span>
#include <unordered_map>
#include <unordered_set>

//! \file BiTypeChecker.h
//! \brief Defines the BiTypeCheckerVisitor, consist of the flow for
//! Bi-Directional Type checking, which allows for synthesizing types,
//! validating consistency, and register types.
namespace sammine_lang {

namespace AST {

/// Outcome of validating a case expression's patterns.
enum class CasePatternResult {
  Valid,
  InvalidArms, ///< Errors reported; arms are still checked where possible.
  Fatal,       ///< Errors reported; the case expression is poisoned.
};

/// What a call expects, as decided by a call resolver.
struct ResolvedCall {
  std::string callee;         ///< Name used in diagnostics.
  std::vector<Type> expected; ///< Expected type of each checked argument.
  Type result;
  std::optional<Type> signature; ///< Shown in a note on argument mismatches.
  bool failed; ///< An error was already reported; the call is poisoned.

  static ResolvedCall failure() {
    return {"", {}, Type::Poisoned(), std::nullopt, true};
  }
};

class BiTypeCheckerVisitor : public ScopedASTVisitor,
                             public TypeCheckerVisitor {
  /// INFO: Ok let's talk about error propagation in this checker.
  /// the synthesize will error if I cannot get something out of id_map or
  /// typename_map (technically shouldn't happen)

  // We're gonna provide look up in different

public:
  ASTProperties &props_;
  // What a name in an expression is bound to. A function and a local can
  // share a name, and the innermost binding decides which one it means.
  struct Binding {
    Type type = Type::NonExistent();
    bool is_function = false;
  };

  // INFO: x, y, z
  LexicalStack<Binding> id_to_type;

  void register_variable(const std::string &name, Type type) {
    id_to_type.registerNameT(name, Binding{std::move(type), false});
  }
  // Functions are top-level definitions, so they are bound in the root scope.
  void register_function(const std::string &name, Type type) {
    id_to_type.registerNameTAtRoot(name, Binding{std::move(type), true});
  }
  std::optional<Type> lookup_type(const std::string &name) const {
    if (auto binding = id_to_type.recursive_try_get(name))
      return binding->type;
    return std::nullopt;
  }
  // Whether `name` is bound to a variable, as opposed to a function, an enum
  // variant (which is not in id_to_type) or nothing.
  bool names_variable(const std::string &name) const {
    auto binding = id_to_type.recursive_try_get(name);
    return binding && !binding->is_function;
  }

  // INFO: i64, f64 bla bla bla
  LexicalStack<Type> typename_to_type;
  TypeMapOrdering type_map_ordering;

  // Monomorphization state: generic registration, dedup, cloning, output
  Monomorphizer monomorphizer;

  // Opens a scope that sees only top-level names. A generic is instantiated
  // from inside the call that needs it, but its body must be checked where
  // it is defined, at the top level, not among the caller's locals.
  class TopLevelScopeGuard {
    BiTypeCheckerVisitor &visitor_;

  public:
    explicit TopLevelScopeGuard(BiTypeCheckerVisitor &visitor)
        : visitor_(visitor) {
      visitor_.push_ast_context();
      visitor_.id_to_type.push_context_under_root();
      visitor_.typename_to_type.push_context_under_root();
    }
    ~TopLevelScopeGuard() { visitor_.exit_new_scope(); }
    TopLevelScopeGuard(const TopLevelScopeGuard &) = delete;
    TopLevelScopeGuard &operator=(const TopLevelScopeGuard &) = delete;
  };

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
  virtual void visit(CastExprAST *ast) override;
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
  virtual void visit(ForExprAST *ast) override;
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

  /// Register a struct or enum type by name, with its fields or variants
  /// filled in later. Returns the type, declaring it only once.
  Type declare_struct(StructDefAST *ast);
  Type declare_enum(EnumDefAST *ast);
  /// Reports and returns true if `type`, written at `where` inside the
  /// definition of `self`, refers back to `self`.
  bool refers_to_itself(TypeExprAST *where, const Type &type,
                        const Type &self);
  /// Type aliases not expanded yet, and the ones being expanded, outermost
  /// first.
  std::unordered_map<std::string, TypeAliasDefAST *> pending_aliases;
  std::vector<TypeAliasDefAST *> alias_path;
  /// Expand the not yet expanded alias `name`, as rustc does when an alias is
  /// first needed. Nullopt if `name` isn't such an alias; poisoned if it
  /// refers to itself, which is reported here.
  std::optional<Type> resolve_pending_alias(const std::string &name);
  /// Register `Alias::Variant` for each variant of an alias of an enum.
  void register_alias_variants(TypeAliasDefAST *ast);

  // Two-pass typeclass registration (called before full type checking)
  void register_typeclass_decl(TypeClassDeclAST *ast);
  void register_typeclass_instance(TypeClassInstanceAST *ast);
  /// True if `key` is an instance the code being checked can use: built in,
  /// written in source, or imported (if private, only by its module's code).
  bool has_instance(const MonomorphizedKey &key);
  /// Instances imported but not exported → their module.
  std::unordered_map<MonomorphizedKey, std::string, MonomorphizedKeyHash>
      private_instances;
  /// The error for the missing instance `key`: that it is private, if it is,
  /// or else `otherwise`.
  std::string missing_instance(const MonomorphizedKey &key,
                               std::string otherwise) const;

  // Monomorphize and type-check the generic function a call resolved to.
  void instantiate_generic_callee(CallExprAST *ast);

  /// The type parameters of the generic struct or enum `base`, or null if
  /// it names neither.
  const std::vector<std::string> *
  generic_type_params(const sammine_util::QualifiedName &base) const;
  /// The generic struct or enum `base` applied to `type_args`: its instance,
  /// monomorphized on first use, or a placeholder while an argument is still
  /// a type parameter, as Vec<T> in a generic signature.
  Type generic_type_instance(const sammine_util::QualifiedName &base,
                             std::vector<Type> type_args);
  /// The type two values have together, as an if's branches, a case's arms
  /// or an array's elements do: TypeMapOrdering::lowest_common_type, or else
  /// the join of two instances of one generic open in different arguments:
  /// Left(1i64), an Either<i64, _>, and Right(2.5), an Either<_, f64>, are
  /// both an Either<i64, f64>. nullopt if they conflict.
  std::optional<Type> common_type(const Type &a, const Type &b);
  /// Reports `expr`, whose type has an open type argument that nothing
  /// decides, as `let x = None;` leaves Option<_>.
  void report_open_type(const ExprAST *expr);

  /// Binds the type parameters `params` by unifying each of `args` with its
  /// parameter in `patterns`, on top of `bindings`. A parameter whose type
  /// is already decided is left for check_call_argument, so a literal takes
  /// that type. Returns the index of the first argument that is poisoned or
  /// doesn't match, for the caller to report.
  std::optional<size_t>
  infer_type_args(std::span<const std::unique_ptr<ExprAST>> args,
                  std::span<const Type> patterns,
                  const std::vector<std::string> &params,
                  TypeBindings &bindings);

  // Call resolvers: nullopt means the call is not of this kind.

  /// The enum `variant` (Color::Red, Option<i32>::None, Option::Some)
  /// belongs to, or nullopt if it names no enum's variant. Poisoned once an
  /// error is reported, at `site`. A generic enum without type arguments is
  /// instantiated with those inferred from `payload`.
  std::optional<Type>
  enum_of_variant(const sammine_util::QualifiedName &variant,
                  const std::vector<std::unique_ptr<TypeExprAST>> &type_args,
                  const ExprAST *site,
                  std::span<const std::unique_ptr<ExprAST>> payload = {});
  Type infer_enum_instance(EnumDefAST *def,
                           const sammine_util::QualifiedName &variant,
                           const ExprAST *site,
                           std::span<const std::unique_ptr<ExprAST>> payload);
  std::optional<ResolvedCall> resolve_enum_call(CallExprAST *ast);
  std::optional<ResolvedCall> resolve_typeclass_call(CallExprAST *ast);
  std::optional<ResolvedCall> resolve_generic_call(CallExprAST *ast);
  std::optional<ResolvedCall> resolve_normal_call(CallExprAST *ast);
  ResolvedCall applied_call(CallExprAST *ast, std::string callee,
                            std::span<const Type> params, Type result,
                            std::optional<Type> signature);

  /// Check one visited call argument against its expected type, resolving
  /// literals on success. Returns false on a mismatch (reported here) or a
  /// poisoned argument.
  bool check_call_argument(CallExprAST *ast, size_t i, const Type &expected,
                           const std::string &callee);

  /// A generic prototype's parameter and return types under concrete bindings.
  std::pair<std::vector<Type>, Type>
  resolve_generic_signature(PrototypeAST *proto, std::span<const Type> params,
                            const Type &return_type,
                            const TypeBindings &bindings);

  /// Resolve explicit type args against type params, producing bindings and
  /// the resolved types in order. Returns nullopt if any type arg resolves
  /// to Poisoned. Caller must check size equality beforehand.
  std::optional<std::pair<TypeBindings, std::vector<Type>>>
  resolve_explicit_type_args(
      const std::vector<std::unique_ptr<TypeExprAST>> &explicit_type_args,
      const std::vector<std::string> &type_params);

  // Report a type mismatch error with an optional incompatibility hint
  void report_type_mismatch(sammine_util::Location loc, const std::string &msg,
                            const Type &expected, const Type &actual) {
    this->add_error(loc, msg);
    if (auto hint = incompatibility_hint(expected, actual))
      this->add_diagnostics(loc, *hint);
  }

  // Binary expression synthesis helper
  bool check_assign_target(BinaryExprAST *ast);
  bool check_addr_of_operand(AddrOfExprAST *ast);
  Type synthesize_binary_operator(BinaryExprAST *ast, const Type &lhs_type,
                                  const Type &rhs_type);

  // Kernel intrinsic synthesis helpers
  Type synthesize_kernel_map(KernelDefAST *kd, KernelMapExprAST *map_expr);
  Type synthesize_kernel_reduce(KernelDefAST *kd,
                                KernelReduceExprAST *reduce_expr);

  /// The array type of a kernel operation's input, or nullopt after reporting
  /// that the name is unknown or not an array.
  std::optional<Type> lookup_kernel_input_array(const std::string &name,
                                                const char *op,
                                                sammine_util::Location loc);

  Type synthesize_tuple_destructure(VarDefAST *ast);

  // Case expression synthesis helpers
  CasePatternResult check_literal_patterns(CaseExprAST *ast,
                                           Type &scrutinee_type);
  CasePatternResult check_enum_patterns(CaseExprAST *ast,
                                        const Type &scrutinee_type);
  Type check_arm_bodies(CaseExprAST *ast, const Type &scrutinee_type,
                        bool &had_error);

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
  virtual Type synthesize(CastExprAST *ast) override;
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
  virtual Type synthesize(ForExprAST *ast) override;
  virtual Type synthesize(TupleLiteralExprAST *ast) override;
  virtual Type synthesize(TypeClassDeclAST *ast) override;
  virtual Type synthesize(TypeClassInstanceAST *ast) override;
  virtual Type synthesize(KernelDefAST *ast) override;

  Type resolve_type_expr(TypeExprAST *type_expr) {
    if (!type_expr)
      return Type::NonExistent();

    if (type_expr->resolved_type)
      return *type_expr->resolved_type;

    if (auto *simple = llvm::dyn_cast<SimpleTypeExprAST>(type_expr)) {
      auto mangled = simple->name.mangled();
      auto get_type_opt = this->get_typename_type(mangled);
      if (!get_type_opt)
        get_type_opt = resolve_pending_alias(mangled);
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
      auto *type_params = generic_type_params(gen->base_name);
      if (!type_params) {
        this->add_error(type_expr->location,
                        fmt::format("'{}' is not a generic type",
                                    gen->base_name.mangled()));
        return Type::Poisoned();
      }
      if (gen->type_args.size() != type_params->size()) {
        this->add_error(
            type_expr->location,
            fmt::format("Generic type '{}' expects {} type argument(s), got {}",
                        gen->base_name.mangled(), type_params->size(),
                        gen->type_args.size()));
        return Type::Poisoned();
      }

      std::vector<Type> type_args;
      for (auto &arg : gen->type_args) {
        auto resolved = resolve_type_expr(arg.get());
        if (resolved.is_poisoned())
          return Type::Poisoned();
        type_args.push_back(std::move(resolved));
      }
      return generic_type_instance(gen->base_name, std::move(type_args));
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

/// Recursively resolve polymorphic types in an expression tree to a concrete
/// target type: numeric literals, and values with an open type argument, as
/// None. Walks through UnaryNeg, BinaryExpr, IfExpr, CaseExpr, ArrayLiteral,
/// TupleLiteral, StructLiteral and enum constructor calls to reach them.
inline void resolve_literal_type(ExprAST *expr, const Type &target) {
  // An open target decides nothing.
  if (!expr || target.is_open())
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
  if (auto *sl = llvm::dyn_cast<StructLiteralExprAST>(expr)) {
    if (target.type_kind == TypeKind::Struct) {
      auto &st = std::get<StructType>(target.type_data);
      for (size_t i = 0; i < sl->field_names.size(); i++) {
        auto idx = st.get_field_index(sl->field_names[i]);
        if (idx.has_value())
          resolve_literal_type(sl->field_values[i].get(),
                               st.get_field_type(idx.value()));
      }
      expr->set_type(target);
    }
    return;
  }

  // Scalar polymorphic literals, and values with an open type argument
  if (!expr->get_type().is_polymorphic())
    return;

  expr->set_type(target);

  if (auto *call = llvm::dyn_cast<CallExprAST>(expr)) {
    // A constructor's payload is decided with it: Some(None) as an
    // Option<Option<i64>>.
    if (target.type_kind == TypeKind::Enum) {
      auto &et = std::get<EnumType>(target.type_data);
      if (auto idx = et.get_variant_index(call->functionName.get_name())) {
        auto &payload = et.get_variant(*idx).payload_types;
        for (size_t i = 0; i < call->arguments.size() && i < payload.size();
             i++)
          resolve_literal_type(call->arguments[i].get(), payload[i]);
      }
    }
  } else if (auto *unary = llvm::dyn_cast<UnaryNegExprAST>(expr)) {
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
  } else if (auto *case_expr = llvm::dyn_cast<CaseExprAST>(expr)) {
    for (auto &arm : case_expr->arms) {
      if (arm.body->Statements.empty() ||
          !arm.body->get_type().is_polymorphic())
        continue;
      resolve_literal_type(arm.body->Statements.back().get(), target);
      arm.body->set_type(target);
    }
  }
}

} // namespace AST
} // namespace sammine_lang
