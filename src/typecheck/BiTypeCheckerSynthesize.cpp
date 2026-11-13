#include "ast/Ast.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "typecheck/BiTypeChecker.h"
#include "typecheck/BuiltinInstances.h"
#include "typecheck/Types.h"
#include "typecheck/UniSubst.h"
#include "llvm/ADT/STLExtras.h"

#define DEBUG_TYPE "typecheck"
#include "util/Logging.h"

//! \file BiTypeCheckerSynthesize.cpp
//! \brief All synthesize() methods, call-dispatch helpers, and
//!        generic unification/substitution for BiTypeCheckerVisitor.
namespace sammine_lang::AST {

/// Format a generic call's display name with resolved type args,
/// e.g. "g::identity<i32>" or "swap<i32, f64>".
static std::string
format_generic_call_name(const sammine_util::QualifiedName &fn_name,
                         const std::vector<std::string> &type_params,
                         const TypeBindings &bindings) {
  std::string result = fn_name.mangled() + "<";
  for (size_t i = 0; i < type_params.size(); i++) {
    if (i > 0)
      result += ", ";
    auto it = bindings.find(type_params[i]);
    result += (it != bindings.end()) ? it->second.to_string() : type_params[i];
  }
  return result + ">";
}

/// The concrete type named by a number literal suffix, if it is a valid one.
static std::optional<Type> number_suffix_type(const std::string &suffix) {
  if (suffix == "i32")
    return Type::I32_t();
  if (suffix == "i64")
    return Type::I64_t();
  if (suffix == "u32")
    return Type::U32_t();
  if (suffix == "u64")
    return Type::U64_t();
  if (suffix == "f32")
    return Type::F32_t();
  if (suffix == "f64")
    return Type::F64_t();
  return std::nullopt;
}

/// Whether `digits` parses as a value of `type`. Reports an error if not.
static bool is_representable_literal(sammine_util::Reportee &reportee,
                                     const std::string &digits,
                                     const Type &type,
                                     sammine_util::Location loc) {
  if (type.is_float())
    return sammine_util::str_to_double(reportee, digits, loc).has_value();
  if (type.type_kind == TypeKind::U64_t && digits.front() != '-')
    return sammine_util::str_to_int<uint64_t>(reportee, digits, loc)
        .has_value();
  return sammine_util::str_to_int<int64_t>(reportee, digits, loc).has_value();
}

/// Validate a raw number lexeme's suffix and value. Reports an error if bad.
static bool is_valid_number_lexeme(sammine_util::Reportee &reportee,
                                   const std::string &lexeme, const Type &type,
                                   sammine_util::Location loc) {
  auto [digits, suffix] = sammine_util::split_number_lexeme(lexeme);
  if (!suffix.empty() && !number_suffix_type(suffix)) {
    reportee.add_error(
        loc, fmt::format("invalid type suffix '{}' on number literal", suffix));
    return false;
  }
  return is_representable_literal(reportee, digits, type, loc);
}

/// Whether a range bound has integer type. Reports an error if not.
static bool is_integer_bound(sammine_util::Reportee &reportee, ExprAST *bound,
                             const Type &type, const char *label) {
  if (type.is_integer())
    return true;
  reportee.add_error(bound->get_location(),
                     fmt::format("{} must be an integer type, got {}", label,
                                 type.to_string()));
  return false;
}

/// Reject a for-range with literal bounds that is empty, comparing as `T`.
template <sammine_util::LiteralInt T>
static bool is_nonempty_for_range(sammine_util::Reportee &reportee,
                                  ForExprAST *ast, NumberExprAST *start,
                                  NumberExprAST *end) {
  auto start_val = sammine_util::str_to_int<T>(reportee, start->number,
                                               start->get_location());
  auto end_val =
      sammine_util::str_to_int<T>(reportee, end->number, end->get_location());
  if (!start_val || !end_val)
    return false;
  if (*start_val < *end_val)
    return true;
  reportee.add_error(ast->get_location(),
                     fmt::format("for-range is empty: start ({}) >= end ({})",
                                 *start_val, *end_val));
  return false;
}

std::optional<std::pair<TypeBindings, std::vector<Type>>>
BiTypeCheckerVisitor::resolve_explicit_type_args(
    const std::vector<std::unique_ptr<TypeExprAST>> &explicit_type_args,
    const std::vector<std::string> &type_params) {
  TypeBindings bindings;
  std::vector<Type> type_args;
  for (size_t i = 0; i < explicit_type_args.size(); i++) {
    auto resolved = resolve_type_expr(explicit_type_args[i].get());
    if (resolved.is_poisoned())
      return std::nullopt;
    bindings[type_params[i]] = resolved;
    type_args.push_back(resolved);
  }
  return std::make_pair(std::move(bindings), std::move(type_args));
}

Type BiTypeCheckerVisitor::synthesize(ProgramAST *ast) {
  return Type::NonExistent();
}

// Variable definition: infers type from expression, checks against annotation
// if present. Polymorphic literals (Integer/Flt) resolve to the annotated type
// via compatible_to_from.
// Tuple destructuring: let (a, b) = expr;
Type BiTypeCheckerVisitor::synthesize_tuple_destructure(VarDefAST *ast) {
  if (!ast->Expression) {
    this->add_error(ast->get_location(),
                    "Destructuring requires an initializer expression");
    return ast->set_type(Type::Poisoned());
  }
  auto expr_type = ast->Expression->accept_synthesis(this);
  if (expr_type.is_poisoned())
    return ast->set_type(Type::Poisoned());

  // What never gives a value, as a panic, gives no tuple to take apart: each
  // name is what its annotation says, or Never. Nothing after it runs.
  if (expr_type.is_never()) {
    for (auto &var : ast->destructure_vars) {
      auto type = var->type_expr ? var->accept_synthesis(this) : Type::Never();
      if (type.is_poisoned())
        return ast->set_type(Type::Poisoned());
      var->set_type(type);
      register_variable(var->name, type);
    }
    return ast->set_type(Type::Unit());
  }

  if (expr_type.type_kind != TypeKind::Tuple) {
    this->add_error(ast->Expression->get_location(),
                    fmt::format("Cannot destructure non-tuple type '{}'",
                                expr_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  auto &tt = std::get<TupleType>(expr_type.type_data);
  if (tt.size() != ast->destructure_vars.size()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Tuple has {} elements but destructuring has {} variables",
                    tt.size(), ast->destructure_vars.size()));
    return ast->set_type(Type::Poisoned());
  }

  // An annotation is the type its element takes, as a let's is for its
  // initializer: (5000000000, None) as an (i64, Option<i64>).
  std::vector<Type> elem_types;
  for (size_t i = 0; i < tt.size(); i++) {
    auto elem_type = tt.get_element(i);
    // If the var has a type annotation, check compatibility
    if (ast->destructure_vars[i]->type_expr) {
      auto ann_type = ast->destructure_vars[i]->accept_synthesis(this);
      if (!type_map_ordering.compatible_to_from(ann_type, elem_type)) {
        report_type_mismatch(
            ast->destructure_vars[i]->get_location(),
            fmt::format("Type annotation '{}' incompatible with tuple "
                        "element type '{}'",
                        ann_type.to_string(), elem_type.to_string()),
            ann_type, elem_type);
        return ast->set_type(Type::Poisoned());
      }
      elem_type = ann_type;
    }
    ast->destructure_vars[i]->set_type(elem_type);
    register_variable(ast->destructure_vars[i]->name, elem_type);
    elem_types.push_back(elem_type);
  }
  resolve_literal_type(ast->Expression.get(), Type::Tuple(std::move(elem_types)));

  return ast->set_type(Type::Unit());
}

Type BiTypeCheckerVisitor::synthesize(VarDefAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  if (ast->is_tuple_destructure)
    return synthesize_tuple_destructure(ast);

  // if you dont have type lexeme for typed var, then just assign type of expr
  // to typed var, if we dont have expr also, then we add error
  //
  // if you do, then just use type lexeme as type of typed var

  if (ast->TypedVar->type_expr != nullptr)
    ast->set_type(ast->TypedVar->accept_synthesis(this));
  else if (ast->Expression) {
    ast->set_type(ast->Expression->accept_synthesis(this));
    // No annotation: default polymorphic literals (Integer→i32, Flt→f64)
    auto concrete = default_polymorphic_type(ast->get_type());
    if (concrete != ast->get_type()) {
      resolve_literal_type(ast->Expression.get(), concrete);
      ast->set_type(concrete);
    }
  } else {
    this->add_error(ast->get_location(),
                    "Variable declared without initializer");
    ast->set_type(Type::Poisoned());
  }

  register_variable(ast->TypedVar->name, ast->get_type());
  LOG({
    fmt::print(stderr, "[typecheck] synthesize VarDefAST: '{}' : {} (immutable{})\n",
               ast->TypedVar->name, ast->get_type().to_string(),
               ast->get_type().is_linear_v() ? ", linear" : "");
  });
  return ast->get_type();
}

Type BiTypeCheckerVisitor::synthesize(ExternAST *ast) {
  return Type::NonExistent();
}
Type BiTypeCheckerVisitor::synthesize(StructDefAST *ast) {
  return Type::NonExistent();
}
Type BiTypeCheckerVisitor::synthesize(EnumDefAST *ast) {
  return Type::NonExistent();
}
Type BiTypeCheckerVisitor::synthesize(TypeAliasDefAST *ast) {
  return props_.type_alias(ast->id()).resolved_type;
}
Type BiTypeCheckerVisitor::synthesize(FuncDefAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  return ast->set_type(ast->Prototype->accept_synthesis(this));
}

Type BiTypeCheckerVisitor::synthesize(PrototypeAST *ast) {
  auto v = std::vector<Type>();
  for (size_t i = 0; i < ast->parameterVectors.size(); i++)
    v.push_back(ast->parameterVectors[i]->accept_synthesis(this));

  if (ast->returnsUnit())
    v.push_back(Type::Unit());
  else
    v.push_back(resolve_type_expr(ast->return_type_expr.get()));
  ast->set_type(Type::Function(std::move(v), ast->is_var_arg));

  LOG({
    fmt::print(stderr, "[typecheck] synthesize PrototypeAST: '{}' -> {}\n",
               ast->functionName.mangled(), ast->get_type().to_string());
  });
  return ast->get_type();
}

bool BiTypeCheckerVisitor::check_call_argument(CallExprAST *ast, size_t i,
                                               const Type &expected,
                                               const std::string &callee) {
  auto *arg = ast->arguments[i].get();
  auto arg_type = arg->get_type();
  // The argument's own error has already been reported.
  if (arg_type.is_poisoned())
    return false;

  bool is_lent = type_map_ordering.lendable_to_from(expected, arg_type);
  if (!is_lent && !type_map_ordering.compatible_to_from(expected, arg_type)) {
    report_type_mismatch(arg->get_location(),
                         fmt::format("Argument {} to '{}': expected {}, got {}",
                                     i + 1, callee, expected.to_string(),
                                     arg_type.to_string()),
                         expected, arg_type);
    return false;
  }

  resolve_literal_type(arg, expected);
  return true;
}

// The function type taking `params` and giving `result`: an instance's
// signature, as resolve_generic_signature resolves it. substitute() would
// leave a generic struct among its types named as the generic is (Vec<T>).
static Type signature_of(const std::vector<Type> &params, const Type &result) {
  auto types = params;
  types.push_back(result);
  return Type::Function(std::move(types));
}

std::pair<std::vector<Type>, Type>
BiTypeCheckerVisitor::resolve_generic_signature(PrototypeAST *proto,
                                                std::span<const Type> params,
                                                const Type &return_type,
                                                const TypeBindings &bindings) {
  // Re-evaluate the type ASTs with concrete bindings in scope, rather than
  // using substitute(), so generic types (e.g. Box<T> -> Box<i32>) get
  // properly instantiated through resolve_type_expr.
  decltype(typename_to_type)::Guard scope(typename_to_type);
  for (auto &[pname, ptype] : bindings)
    typename_to_type.registerNameT(bare(pname), ptype);

  std::vector<Type> expected;
  for (size_t i = 0; i < params.size(); i++) {
    auto *param_type_ast = proto->parameterVectors[i]->type_expr.get();
    expected.push_back(param_type_ast ? resolve_type_expr(param_type_ast)
                                      : substitute(params[i], bindings));
  }

  auto *return_type_ast = proto->return_type_expr.get();
  return {std::move(expected), return_type_ast
                                   ? resolve_type_expr(return_type_ast)
                                   : substitute(return_type, bindings)};
}

// Call resolution order: enum variant → typeclass method → generic function →
// normal call. Each resolver only says what the call expects; the arguments
// are compared here, in one place.
Type BiTypeCheckerVisitor::synthesize(CallExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  // Visit every argument up front, so the resolvers can read argument types
  // and arguments are still visited when resolution fails.
  for (auto &arg : ast->arguments)
    arg->accept_vis(this);

  std::optional<ResolvedCall> call;
  for (auto resolve : {&BiTypeCheckerVisitor::resolve_enum_call,
                       &BiTypeCheckerVisitor::resolve_typeclass_call,
                       &BiTypeCheckerVisitor::resolve_generic_call,
                       &BiTypeCheckerVisitor::resolve_normal_call}) {
    call = (this->*resolve)(ast);
    if (call)
      break;
  }
  if (!call || call->failed)
    return ast->set_type(Type::Poisoned());

  bool args_ok = true;
  for (size_t i = 0; i < call->expected.size(); i++) {
    if (check_call_argument(ast, i, call->expected[i], call->callee))
      continue;
    args_ok = false;
    if (call->signature && !ast->arguments[i]->get_type().is_poisoned())
      this->add_diagnostics(ast->arguments[i]->get_location(),
                            fmt::format("note: '{}' has signature: {}",
                                        call->callee,
                                        call->signature->to_string()));
  }
  ast->set_type(args_ok ? call->result : Type::Poisoned());
  instantiate_generic_callee(ast);
  return ast->get_type();
}

std::optional<size_t> BiTypeCheckerVisitor::infer_type_args(
    std::span<const std::unique_ptr<ExprAST>> args,
    std::span<const Type> patterns, const std::vector<std::string> &params,
    TypeBindings &bindings) {
  auto count = std::min(args.size(), patterns.size());
  // A literal argument says least of the type it is: 2 is any integer. The
  // other arguments bind first, so fold(2, 1000, 2, longer) takes T from
  // longer, and only a literal nothing else decides defaults, as 2 to i32.
  auto is_literal = [&](size_t i) {
    auto arg_type = args[i]->get_type();
    return default_polymorphic_type(arg_type) != arg_type;
  };
  auto bind = [&](size_t i) {
    bool decided = std::ranges::none_of(params, [&](const std::string &tp) {
      return !bindings.contains(tp) && contains_type_param(patterns[i], tp);
    });
    if (decided)
      return true;
    auto arg_type = default_polymorphic_type(args[i]->get_type());
    resolve_literal_type(args[i].get(), arg_type);
    return unify(patterns[i], arg_type, bindings);
  };

  for (size_t i = 0; i < count; i++) {
    if (args[i]->get_type().is_poisoned())
      return i;
    if (!is_literal(i) && !bind(i))
      return i;
  }
  for (size_t i = 0; i < count; i++)
    if (is_literal(i) && !bind(i))
      return i;
  return std::nullopt;
}

std::optional<Type> BiTypeCheckerVisitor::enum_of_variant(
    const sammine_util::QualifiedName &variant,
    const std::vector<std::unique_ptr<TypeExprAST>> &type_args,
    const ExprAST *site, std::span<const std::unique_ptr<ExprAST>> payload) {
  if (!variant.is_qualified())
    return std::nullopt;

  // A concrete enum, or an instance named in full: Color, Option<i32>.
  auto enum_type = get_typename_type(variant.qualifier());

  if (!enum_type) {
    auto *generic_def = monomorphizer.generic_enums.find(variant.qualifier());
    if (generic_def && type_args.empty())
      enum_type = infer_enum_instance(generic_def, variant, site, payload);
    else if (generic_def &&
             type_args.size() == generic_def->type_params.size()) {
      if (auto resolved =
              resolve_explicit_type_args(type_args, generic_def->type_params))
        enum_type =
            generic_type_instance(generic_def->enum_name, resolved->second);
    }
  }

  // A variant of an alias, as IntOption::Some.
  if (!enum_type) {
    auto vc_it = variant_constructors.find(variant);
    if (vc_it != variant_constructors.end())
      enum_type = vc_it->second.first;
  }

  if (!enum_type || (!enum_type->is_poisoned() &&
                     enum_type->type_kind != TypeKind::Enum))
    return std::nullopt;
  return enum_type;
}

// Option::Some(3i64) is Option<i64>::Some: the instance of `def` whose type
// arguments come from the payload, as a generic call's come from its
// arguments. Poisoned once an error is reported, at `site`.
Type BiTypeCheckerVisitor::infer_enum_instance(
    EnumDefAST *def, const sammine_util::QualifiedName &variant,
    const ExprAST *site, std::span<const std::unique_ptr<ExprAST>> payload) {
  auto variant_name = variant.get_name();
  auto variant_def =
      std::ranges::find(def->variants, variant_name, &EnumVariantDef::name);
  if (variant_def == def->variants.end()) {
    add_error(site->get_location(),
              fmt::format("Type '{}' has no variant '{}'",
                          def->enum_name.mangled(), variant_name));
    return Type::Poisoned();
  }

  // The payload as the generic declares it: Some(T).
  std::vector<Type> patterns;
  {
    decltype(typename_to_type)::Guard scope(typename_to_type);
    for (auto &tp : def->type_params)
      typename_to_type.registerNameT(bare(tp), Type::TypeParam(tp));
    for (auto &pt : variant_def->payload_types) {
      auto pattern = resolve_type_expr(pt.get());
      if (pattern.is_poisoned())
        return pattern;
      patterns.push_back(std::move(pattern));
    }
  }

  TypeBindings bindings;

  auto written = variant.mangled();
  if (auto bad = infer_type_args(payload, patterns, def->type_params,
                                 bindings)) {
    auto *arg = payload[*bad].get();
    if (arg->get_type().is_poisoned())
      return Type::Poisoned();
    add_error(arg->get_location(),
              fmt::format("Type mismatch in argument {} of '{}': expected {}, "
                          "got {}",
                          *bad + 1, written,
                          substitute(patterns[*bad], bindings).to_string(),
                          default_polymorphic_type(arg->get_type())
                              .to_string()));
    return Type::Poisoned();
  }

  // What the payload doesn't say stays open for the context to decide:
  // None is an Option<_>.
  for (auto &tp : def->type_params)
    if (!bindings.contains(tp))
      bindings[tp] = Type::Hole();

  return generic_type_instance(def->enum_name,
                               MonomorphizedKey::from_bindings(
                                   def->enum_name, def->type_params, bindings)
                                   .type_args);
}

std::optional<ResolvedCall>
BiTypeCheckerVisitor::resolve_enum_call(CallExprAST *ast) {
  // INFO: All enum variant calls arrive here in qualified form (Enum::Variant).
  // The scope generator rewrites unqualified variant names (e.g. Some(42)) to
  // qualified form (e.g. Option::Some(42)) via variant_to_enum. The type
  // checker does NOT resolve unqualified enum variant names — that is the scope
  // generator's responsibility.
  auto enum_type_opt = enum_of_variant(
      ast->functionName, ast->explicit_type_args, ast, ast->arguments);
  if (!enum_type_opt)
    return std::nullopt;
  if (enum_type_opt->is_poisoned())
    return ResolvedCall::failure();

  auto &et = std::get<EnumType>(enum_type_opt->type_data);
  auto variant_idx = et.get_variant_index(ast->functionName.get_name());
  if (!variant_idx) {
    this->add_error(ast->get_location(),
                    fmt::format("Type '{}' has no variant '{}'",
                                ast->functionName.get_qualifier(),
                                ast->functionName.get_name()));
    return ResolvedCall::failure();
  }
  auto &vi = et.get_variant(*variant_idx);

  if (ast->arguments.size() != vi.payload_types.size()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Enum variant '{}::{}' expects {} arguments, got {}",
                    ast->functionName.get_qualifier(), vi.name,
                    vi.payload_types.size(), ast->arguments.size()));
    return ResolvedCall::failure();
  }

  props_.call(ast->id()).is_enum_constructor = true;
  props_.call(ast->id()).enum_variant_index = *variant_idx;
  return ResolvedCall{
      fmt::format("{}::{}", ast->functionName.get_qualifier(), vi.name),
      vi.payload_types, *enum_type_opt, std::nullopt, false};
}

std::optional<ResolvedCall>
BiTypeCheckerVisitor::resolve_typeclass_call(CallExprAST *ast) {
  // Support both syntaxes:
  //   add<i32>(x, y)       — method_name = "add", type arg in
  //   explicit_type_args Add<i32>::add(x, y)  — method_name = "add"
  //   (get_name()), class from qualifier
  std::string method_name;
  std::string class_name;

  if (ast->functionName.is_qualified()) {
    // Qualified syntax: Add<i32>::add — extract method from last part
    method_name = ast->functionName.get_name();
    class_name = ast->functionName.get_qualifier();
  }

  if (class_name.empty()) {
    // Unqualified syntax: add<i32>(x, y) — look up method in method_to_class
    method_name = ast->functionName.get_name();
    auto class_it = method_to_class.find(method_name);
    if (class_it == method_to_class.end())
      return std::nullopt;
    class_name = class_it->second;
  }

  auto tc_it = type_class_defs.find(class_name);
  if (tc_it == type_class_defs.end())
    return std::nullopt;
  auto *tc = tc_it->second;

  if (ast->explicit_type_args.size() != tc->type_params.size()) {
    add_error(ast->get_location(),
              fmt::format("Type class '{}' expects {} type argument(s), got {}",
                          class_name, tc->type_params.size(),
                          ast->explicit_type_args.size()));
    return ResolvedCall::failure();
  }

  auto resolved_args =
      resolve_explicit_type_args(ast->explicit_type_args, tc->type_params);
  if (!resolved_args)
    return ResolvedCall::failure();
  auto &[tc_bindings, tc_type_args] = *resolved_args;

  auto tc_key = MonomorphizedKey::instance(class_name, tc_type_args);
  if (!has_instance(tc_key)) {
    add_error(ast->get_location(),
              missing_instance(tc_key,
                               fmt::format("No instance of {}<{}>", class_name,
                                           tc_key.type_args_string())));
    return ResolvedCall::failure();
  }

  PrototypeAST *method_proto = nullptr;
  for (auto &p : tc->methods) {
    if (p->functionName.get_name() == method_name) {
      method_proto = p.get();
      break;
    }
  }
  if (!method_proto) {
    add_error(ast->get_location(),
              fmt::format("Method '{}' not found in type class '{}'",
                          method_name, class_name));
    return ResolvedCall::failure();
  }

  auto func_type = std::get<FunctionType>(method_proto->get_type().type_data);
  auto params = func_type.get_params_types();
  if (ast->arguments.size() != params.size()) {
    add_error(ast->get_location(),
              fmt::format("Type class method '{}' expects {} arguments, got {}",
                          method_name, params.size(), ast->arguments.size()));
    return ResolvedCall::failure();
  }

  auto resolved_name = tc_key.to_typeclass_name(method_name);
  props_.call(ast->id()).resolved_name = resolved_name;
  props_.call(ast->id()).is_typeclass_call = true;
  if (is_builtin_instance(tc_key))
    props_.call(ast->id()).builtin_instance = tc_key;
  auto [expected, result] = resolve_generic_signature(
      method_proto, params, func_type.get_return_type(), tc_bindings);
  // The instance's signature, as for any other call: the linear checker reads
  // it to tell a lent argument from a moved one.
  auto instance_type = signature_of(expected, result);
  props_.call(ast->id()).callee_func_type = instance_type;
  return ResolvedCall{resolved_name.mangled(), std::move(expected),
                      std::move(result), std::move(instance_type), false};
}

std::optional<ResolvedCall>
BiTypeCheckerVisitor::resolve_generic_call(CallExprAST *ast) {
  auto *generic_def = monomorphizer.generic_funcs.find(ast->functionName);
  if (!generic_def)
    return std::nullopt;
  auto generic_type = generic_def->get_type();
  auto func = std::get<FunctionType>(generic_type.type_data);
  auto params = func.get_params_types();
  auto &type_params = generic_def->Prototype->type_params;

  // Fewer arguments is a partial application, as for a normal function.
  if (ast->arguments.size() > params.size()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Generic function '{}' expects {} arguments, got {}",
                    ast->functionName.mangled(), params.size(),
                    ast->arguments.size()));
    return ResolvedCall::failure();
  }
  bool is_partial = ast->arguments.size() < params.size();

  TypeBindings bindings;

  if (!ast->explicit_type_args.empty()) {
    // Explicit type arguments: f<T, U>(args) — must provide all
    if (ast->explicit_type_args.size() != type_params.size()) {
      this->add_error(
          ast->get_location(),
          fmt::format("Expected {} type argument(s) for '{}', got {}",
                      type_params.size(), ast->functionName.mangled(),
                      ast->explicit_type_args.size()));
      return ResolvedCall::failure();
    }

    auto resolved_args =
        resolve_explicit_type_args(ast->explicit_type_args, type_params);
    if (!resolved_args)
      return ResolvedCall::failure();
    bindings = std::move(resolved_args->first);
  } else {
    // Infer type arguments from call arguments
    if (auto bad = infer_type_args(ast->arguments, params, type_params,
                                   bindings)) {
      auto i = *bad;
      auto arg_type = ast->arguments[i]->get_type();
      if (arg_type.is_poisoned())
        return ResolvedCall::failure();
      auto call_name =
          format_generic_call_name(ast->written_name, type_params, bindings);
      auto expected = substitute(params[i], bindings);
      this->add_error(ast->arguments[i]->get_location(),
                      fmt::format("Type mismatch in argument {} of '{}': "
                                  "expected {}, got {}",
                                  i + 1, call_name, expected.to_string(),
                                  default_polymorphic_type(arg_type)
                                      .to_string()));
      auto mono_sig = substitute(generic_type, bindings);
      this->add_diagnostics(ast->arguments[i]->get_location(),
                            fmt::format("note: '{}' has signature: {}",
                                        call_name, mono_sig.to_string()));
      return ResolvedCall::failure();
    }

    // Check all type params resolved. A partial application infers them
    // from the arguments it binds alone. An open argument, as None, binds
    // nothing it leaves open: wrap(None) says nothing of T.
    for (auto &tp : type_params) {
      if (!bindings.contains(tp) || bindings.at(tp).is_open()) {
        this->add_error(
            ast->get_location(),
            is_partial
                ? fmt::format("Type parameter '{}' could not be inferred for "
                              "'{}' from the arguments it is partially "
                              "applied to; give it explicitly",
                              tp, ast->functionName.mangled())
                : fmt::format("Type parameter '{}' could not be inferred for "
                              "'{}'",
                              tp, ast->functionName.mangled()));
        return ResolvedCall::failure();
      }
    }
  }

  auto key =
      MonomorphizedKey::from_bindings(ast->functionName, type_params, bindings);

  props_.call(ast->id()).resolved_name = key.generic_name();
  props_.call(ast->id()).type_bindings = bindings;
  auto [expected, result] = resolve_generic_signature(
      generic_def->Prototype.get(), params, func.get_return_type(), bindings);
  // The instance's signature: what the call passes and gets back, as the
  // linear checker and codegen read it.
  auto instance_type = signature_of(expected, result);
  props_.call(ast->id()).callee_func_type = instance_type;
  return applied_call(
      ast, format_generic_call_name(ast->written_name, type_params, bindings),
      expected, std::move(result), std::move(instance_type));
}

std::optional<ResolvedCall>
BiTypeCheckerVisitor::resolve_normal_call(CallExprAST *ast) {
  auto ty = lookup_type(ast->functionName);
  if (!ty.has_value()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Function '{}' not found", ast->functionName.mangled()));
    return ResolvedCall::failure();
  }

  props_.call(ast->id()).callee_func_type = ty;

  // Already reported, e.g. a variadic function bound to a variable.
  if (ty->is_poisoned())
    return ResolvedCall::failure();
  if (ty->type_kind != TypeKind::Function) {
    this->add_error(
        ast->get_location(),
        fmt::format("'{}' is not callable", ast->functionName.mangled()));
    return ResolvedCall::failure();
  }

  auto func = std::get<FunctionType>(ty->type_data);
  auto params = func.get_params_types();
  auto callee = ast->written_name.mangled();

  // Variadic functions: accept any number of args >= fixed params
  if (func.is_var_arg()) {
    if (ast->arguments.size() < params.size()) {
      this->add_error(ast->get_location(),
                      fmt::format("Variadic function '{}' requires at least {} "
                                  "arguments, got {}",
                                  ast->functionName.mangled(), params.size(),
                                  ast->arguments.size()));
      return ResolvedCall::failure();
    }
    // A pointer passed through `...` has no declared parameter type, so
    // nothing says C only reads it. sammine memory is immutable: C hands
    // back what it produces instead of writing into ours.
    bool lends_address = false;
    bool passes_function = false;
    for (auto &arg : llvm::drop_begin(ast->arguments, params.size())) {
      if (llvm::isa<AddrOfExprAST>(arg.get())) {
        this->add_error(arg->get_location(),
                        fmt::format("Cannot pass an address through the '...' "
                                    "of '{}': C could write through it",
                                    ast->functionName.mangled()));
        lends_address = true;
      }
      // Nothing declares it a function, so it would go as sammine has it,
      // a closure, not as C takes one.
      if (arg->get_type().type_kind == TypeKind::Function) {
        this->add_error(arg->get_location(),
                        fmt::format("Cannot pass a function through the '...' "
                                    "of '{}': declare the parameter, so C "
                                    "gets a pointer to its code",
                                    ast->functionName.mangled()));
        passes_function = true;
      }
    }
    if (lends_address || passes_function || !check_c_call(ast, params))
      return ResolvedCall::failure();
    return ResolvedCall{callee, {}, func.get_return_type(), ty, false};
  }

  if (ast->arguments.size() > params.size()) {
    this->add_error(ast->get_location(),
                    fmt::format("Function '{}' expects {} arguments, got {}",
                                ast->functionName.mangled(), params.size(),
                                ast->arguments.size()));
    return ResolvedCall::failure();
  }
  if (!check_c_call(ast, params))
    return ResolvedCall::failure();

  return applied_call(ast, callee, params, func.get_return_type(), ty);
}

Type BiTypeCheckerVisitor::synthesize_generic_function_value(
    VariableExprAST *ast, FuncDefAST *generic) {
  auto name = ast->variableName.mangled();
  auto &type_params = generic->Prototype->type_params;
  // Nothing says what its types are: a value has no arguments to infer them
  // from, and one copy of the function is made per set of types.
  if (ast->explicit_type_args.empty()) {
    this->add_error(ast->get_location(),
                    fmt::format("'{}' is generic, so it is used as a value "
                                "with its types given, as '{}<{}>'",
                                name, name, fmt::join(type_params, ", ")));
    return ast->set_type(Type::Poisoned());
  }
  if (ast->explicit_type_args.size() != type_params.size()) {
    this->add_error(ast->get_location(),
                    fmt::format("Expected {} type argument(s) for '{}', got {}",
                                type_params.size(), name,
                                ast->explicit_type_args.size()));
    return ast->set_type(Type::Poisoned());
  }
  auto resolved =
      resolve_explicit_type_args(ast->explicit_type_args, type_params);
  if (!resolved)
    return ast->set_type(Type::Poisoned());
  auto &bindings = resolved->first;

  auto func = std::get<FunctionType>(generic->get_type().type_data);
  auto [params, result] = resolve_generic_signature(
      generic->Prototype.get(), func.get_params_types(), func.get_return_type(),
      bindings);
  // The copy is made as a call to it makes it (instantiate_generic_callee).
  auto key =
      MonomorphizedKey::from_bindings(ast->variableName, type_params, bindings);
  props_.variable(ast->id()).resolved_function = key.generic_name();
  if (auto *copy = monomorphizer.try_instantiate_func(generic, key, bindings)) {
    TopLevelScopeGuard scope(*this);
    copy->accept_vis(this);
  }
  return ast->set_type(signature_of(params, result));
}

bool BiTypeCheckerVisitor::check_c_call(CallExprAST *ast,
                                        std::span<const Type> params) {
  auto name = ast->functionName.mangled();
  if (!takes_c_callback(ast->functionName))
    return true;
  // A partial application is a closure, which calls C through sammine's
  // calling convention: the function it is given would reach C as one too.
  if (ast->arguments.size() < params.size()) {
    this->add_error(ast->get_location(),
                    fmt::format("'{}' gives a function to C, so it can't be "
                                "partially applied: pass all {} arguments",
                                ast->written_name.mangled(), params.size()));
    return false;
  }
  bool ok = true;
  for (auto [arg, param] : llvm::zip(ast->arguments, params)) {
    if (param.type_kind != TypeKind::Function)
      continue;
    // C calls a function through a pointer to its code, with nothing bound:
    // a function named here has one. A closure or a binding holding a
    // function value is code and an environment, and C has nowhere to put
    // the environment.
    auto *var = llvm::dyn_cast<VariableExprAST>(arg.get());
    if (var && !names_variable(var->variableName) &&
        var->get_type().type_kind == TypeKind::Function)
      continue;
    this->add_error(arg->get_location(),
                    fmt::format("'{}' gives this function to C, which calls "
                                "it through a pointer to its code: pass a "
                                "function by name, not a closure or a "
                                "function value",
                                ast->written_name.mangled()));
    ok = false;
  }
  return ok;
}

// A call of a function taking `params` and returning `result`, given at most
// as many arguments. Given fewer, it is a partial application: the call
// makes a closure that takes the parameters left over.
ResolvedCall BiTypeCheckerVisitor::applied_call(CallExprAST *ast,
                                                std::string callee,
                                                std::span<const Type> params,
                                                Type result,
                                                std::optional<Type> signature) {
  auto given = ast->arguments.size();
  auto supplied = params.first(given);
  std::vector<Type> expected(supplied.begin(), supplied.end());
  if (given < params.size()) {
    props_.call(ast->id()).is_partial = true;
    auto rest = params.subspan(given);
    std::vector<Type> remaining(rest.begin(), rest.end());
    remaining.push_back(std::move(result));
    result = Type::Function(std::move(remaining));
  }
  return ResolvedCall{std::move(callee), std::move(expected),
                      std::move(result), std::move(signature), false};
}

Type BiTypeCheckerVisitor::synthesize(ReturnStmtAST *ast) {
  return ast->set_type(Type::Never());
}
// Binary ops: resolved via typeclass instances (e.g. Add<i32>::add for `+`).
// Comparison ops return bool. Assignment checks mutability. Logical ops require
// bool.
Type BiTypeCheckerVisitor::synthesize(BinaryExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  // Ensure children are visited — synthesis may be called directly (e.g.
  // from enum constructor args) without a prior visit(BinaryExprAST*).
  if (!ast->LHS->synthesized())
    ast->LHS->accept_vis(this);
  if (!ast->RHS->synthesized())
    ast->RHS->accept_vis(this);

  // Checked before the poisoned-operand return: an invalid target is its own
  // error, e.g. an out-of-bounds write to an immutable array is both.
  if (ast->Op->is_assign() && !check_assign_target(ast))
    return ast->set_type(Type::Poisoned());

  if (ast->LHS->get_type().is_poisoned() || ast->RHS->get_type().is_poisoned())
    return ast->set_type(Type::Poisoned());

  if (ast->LHS->get_type().is_never() || ast->RHS->get_type().is_never())
    return ast->set_type(Type::Never());

  // Both operands polymorphic and same kind: keep polymorphic, skip typeclass
  // But NOT for comparison/logical operators, which must return Bool
  if (ast->LHS->get_type().is_polymorphic_numeric() &&
      ast->RHS->get_type().is_polymorphic_numeric() &&
      ast->LHS->get_type().type_kind == ast->RHS->get_type().type_kind &&
      !ast->Op->is_comparison() && !ast->Op->is_logical()) {
    return ast->set_type(ast->LHS->get_type());
  }
  // One polymorphic, one concrete: resolve polymorphic to concrete
  if (ast->LHS->get_type().is_polymorphic() &&
      !ast->RHS->get_type().is_polymorphic()) {
    if (type_map_ordering.structurally_compatible(ast->RHS->get_type(),
                                                  ast->LHS->get_type()))
      resolve_literal_type(ast->LHS.get(), ast->RHS->get_type());
  } else if (ast->RHS->get_type().is_polymorphic() &&
             !ast->LHS->get_type().is_polymorphic()) {
    if (type_map_ordering.structurally_compatible(ast->LHS->get_type(),
                                                  ast->RHS->get_type()))
      resolve_literal_type(ast->RHS.get(), ast->LHS->get_type());
  }
  // The operator's instance is chosen by the operand types, so an open one
  // can't wait for a context: None == None.
  for (auto *operand : {ast->LHS.get(), ast->RHS.get()})
    if (operand->get_type().is_open()) {
      report_open_type(operand);
      return ast->set_type(Type::Poisoned());
    }

  if (!this->type_map_ordering.compatible_to_from(ast->LHS->get_type(),
                                                  ast->RHS->get_type())) {
    report_type_mismatch(
        ast->Op->get_location(),
        fmt::format("Incompatible types for operator '{}': {} and {}",
                    ast->Op->lexeme, ast->LHS->get_type().to_string(),
                    ast->RHS->get_type().to_string()),
        ast->LHS->get_type(), ast->RHS->get_type());
    return ast->set_type(Type::Poisoned());
  }

  return synthesize_binary_operator(ast, ast->LHS->get_type(),
                                    ast->RHS->get_type());
}

// Where a write to `target` lands: an array element or a field is written
// into its base, while `*p` and `p[i]` reach memory through the pointer `p`.
static const ExprAST *write_root(const ExprAST *target) {
  if (auto *idx = llvm::dyn_cast<IndexExprAST>(target);
      idx && idx->array_expr->get_type().type_kind == TypeKind::Array)
    return write_root(idx->array_expr.get());
  if (auto *field = llvm::dyn_cast<FieldAccessExprAST>(target))
    return write_root(field->object_expr.get());
  return target;
}

void BiTypeCheckerVisitor::check_unsafe_allowed(sammine_util::Location loc,
                                                std::string_view what) {
  auto *fn = ctx().enclosing_function;
  if (fn && fn->Prototype->is_unsafe)
    return;
  this->add_error(loc, fmt::format("{} is only allowed in an unsafe function",
                                   what));
  if (fn)
    this->add_diagnostics(
        fn->Prototype->get_location(),
        fmt::format("declare it as 'let unsafe {}' to allow it",
                    fn->Prototype->functionName.get_name()));
}

// Bindings are immutable, so an assignment must land in memory behind a
// pointer. Reports the assignment otherwise, based on where it lands.
bool BiTypeCheckerVisitor::check_assign_target(BinaryExprAST *ast) {
  auto *target = ast->LHS.get();
  auto *root = write_root(target);
  if (auto *idx = llvm::dyn_cast<IndexExprAST>(root);
      idx && props_.index(idx->id()).indexer_method) {
    this->add_error(
        root->get_location(),
        fmt::format("Cannot write into '{}' by index: its Indexer only reads",
                    idx->array_expr->get_type().to_string()));
    return false;
  }
  if (llvm::isa<DerefExprAST, IndexExprAST>(root))
    return true;
  // A root whose type is unknown has already been reported.
  if (root->get_type().is_poisoned())
    return false;

  std::string message;
  if (auto *var = llvm::dyn_cast<VariableExprAST>(root)) {
    message =
        root == target
            ? fmt::format("Cannot reassign variable '{}'. All bindings are "
                          "immutable; use 'let {} = ...' to shadow",
                          var->variableName.mangled(),
                          var->variableName.mangled())
            : fmt::format("Cannot write into '{}'. All bindings are "
                          "immutable; only memory behind a pointer can be "
                          "written",
                          var->variableName.mangled());
  } else {
    message = "Cannot assign to this expression. Only memory behind a "
              "pointer can be written, e.g. '*p', 'p[i]' or '(*p).x'";
  }
  this->add_error(ast->Op->get_location(), message);
  return false;
}

Type BiTypeCheckerVisitor::synthesize_binary_operator(BinaryExprAST *ast,
                                                      const Type &lhs_type,
                                                      const Type &rhs_type) {
  if (ast->Op->is_assign())
    return ast->set_type(Type::Unit());

  if (ast->Op->is_logical()) {
    if (lhs_type == Type::Bool() && rhs_type == Type::Bool())
      return ast->set_type(lhs_type);
    else  {
      add_error(ast->Op->get_location(), fmt::format("Logical operations need boolean arguments"));
      return ast->set_type(Type::Poisoned());
   }
  }

  // Bitwise operators: valid on integer types and integer-backed enums
  if (ast->Op->is_bitwise()) {
    bool valid = lhs_type.is_integer();
    if (!valid && lhs_type.type_kind == TypeKind::Enum) {
      auto &et = std::get<EnumType>(lhs_type.type_data);
      valid = et.is_integer_backed();
    }
    if (!valid) {
      add_error(ast->Op->get_location(),
                fmt::format("Bitwise operator '{}' is not valid on type '{}'",
                            ast->Op->lexeme, lhs_type.to_string()));
      return ast->set_type(Type::Poisoned());
    }
    return ast->set_type(lhs_type);
  }

  // Arithmetic and comparison operators dispatch through typeclasses: + through
  // Add, < through Less. A comparison gives a bool.
  if (auto class_name = class_of_operator(ast->Op->tok_type)) {
    auto tc_key = MonomorphizedKey::instance(*class_name, {lhs_type});
    if (!has_instance(tc_key)) {
      // Only numbers, char and the types with a declared instance, as
      // str::String, have one: any other type declares its own.
      add_error(ast->Op->get_location(),
                missing_instance(
                    tc_key, fmt::format("No instance of {}<{}> — cannot use "
                                        "'{}' on this type; declare one: {}",
                                        *class_name, lhs_type.to_string(),
                                        ast->Op->lexeme,
                                        instance_template(tc_key))));
      return ast->set_type(Type::Poisoned());
    }
    // A built-in instance is an instruction; any other is called.
    if (!is_builtin_instance(tc_key))
      props_.binary(ast->id()).resolved_op_method =
          tc_key.to_typeclass_name(tc_key.class_name());
    return ast->set_type(ast->Op->is_comparison() ? Type::Bool() : lhs_type);
  }

  return ast->set_type(lhs_type);
}

Type BiTypeCheckerVisitor::synthesize(StringExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();
  // String literals are char pointers at runtime
  return ast->set_type(Type::Pointer(Type::Char()));
}

Type BiTypeCheckerVisitor::synthesize(NumberExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  std::optional<Type> type;
  if (ast->suffix.empty())
    type = ast->number.find('.') == std::string::npos ? Type::Integer()
                                                      : Type::Flt();
  else
    type = number_suffix_type(ast->suffix);

  if (!type) {
    this->add_error(
        ast->get_location(),
        fmt::format("invalid type suffix '{}' on number literal", ast->suffix));
    return ast->set_type(Type::Poisoned());
  }

  // Reject unrepresentable values here so codegen can parse them unchecked.
  if (!is_representable_literal(*this, ast->number, *type, ast->get_location()))
    return ast->set_type(Type::Poisoned());
  return ast->set_type(*type);
}
Type BiTypeCheckerVisitor::synthesize(BoolExprAST *ast) {
  return ast->set_type(Type::Bool());
}
Type BiTypeCheckerVisitor::synthesize(CharExprAST *ast) {
  return ast->set_type(Type::Char());
}
Type BiTypeCheckerVisitor::synthesize(VariableExprAST *ast) {
  if (auto *generic = monomorphizer.generic_funcs.find(ast->variableName);
      generic && !names_variable(ast->variableName))
    return synthesize_generic_function_value(ast, generic);
  if (auto ty = lookup_type(ast->variableName)) {
    // A function value only carries the declared parameters, so the extra
    // arguments of a variadic C function would be lost.
    if (ty->type_kind == TypeKind::Function &&
        std::get<FunctionType>(ty->type_data).is_var_arg()) {
      this->add_error(ast->get_location(),
                      fmt::format("'{}' is variadic, so it can't be used as a "
                                  "value; call it directly",
                                  ast->variableName.mangled()));
      return ast->set_type(Type::Poisoned());
    }
    // Called through a function value, it would be given closures where C
    // takes pointers to code (check_c_call).
    if (takes_c_callback(ast->variableName)) {
      this->add_error(ast->get_location(),
                      fmt::format("'{}' gives a function to C, so it can't be "
                                  "used as a value; call it directly",
                                  ast->variableName.mangled()));
      return ast->set_type(Type::Poisoned());
    }
    ast->set_type(*ty);
  } else if (auto enum_type = enum_of_variant(ast->variableName,
                                              ast->explicit_type_args, ast)) {
    if (enum_type->is_poisoned())
      return ast->set_type(Type::Poisoned());
    // An enum variant, e.g. Color::Red or Option<i32>::None. The scope
    // generator has qualified an unqualified one like Red.
    auto &et = std::get<EnumType>(enum_type->type_data);
    auto variant_idx = et.get_variant_index(ast->variableName.get_name());
    if (!variant_idx) {
      this->add_error(ast->get_location(),
                      fmt::format("Type '{}' has no variant '{}'",
                                  et.get_name().mangled(),
                                  ast->variableName.get_name()));
      return ast->set_type(Type::Poisoned());
    }
    auto &vi = et.get_variant(*variant_idx);
    // A variant with a payload needs its arguments, e.g. Circle(1).
    if (!vi.payload_types.empty()) {
      this->add_error(
          ast->get_location(),
          fmt::format("Enum variant '{}' expects {} arguments, got 0",
                      // With its types, once they are known: Option<i32>::Some.
                      enum_type->is_open()
                          ? ast->variableName.mangled()
                          : et.get_name().mangled() + "::" + vi.name,
                      vi.payload_types.size()));
      return ast->set_type(Type::Poisoned());
    }
    props_.variable(ast->id()).is_enum_unit_variant = true;
    props_.variable(ast->id()).enum_variant_index = *variant_idx;
    ast->set_type(*enum_type);
  } else {
    this->add_error(
        ast->get_location(),
        fmt::format("Variable '{}' not found", ast->variableName.mangled()));
    ast->set_type(Type::Poisoned());
  }
  LOG({
    fmt::print(stderr, "[typecheck] synthesize VariableExprAST: '{}' -> {}\n",
               ast->variableName.mangled(), ast->get_type().to_string());
  });
  return ast->get_type();
}
Type BiTypeCheckerVisitor::synthesize(BlockAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  // Block typing rule:
  // 1. Type each statement in order
  // 2. If any statement has type ! (Never) and is not the return , stop: the
  // block's type is !
  // 3. Otherwise, the block's type is the type of its last expression, if
  // the block ends in one: `{ a; b }`
  // 4. If it ends in a statement, `{ a; b; }`, or in nothing, it is ()

  if (ast->Statements.empty()) {
    return ast->set_type(Type::Unit());
  }

  // A block is a scope, so names declared in it end with it.
  ScopeGuard scope(*this);

  // Statements are visited so declarations and returns run their checks.
  bool is_never = false;
  for (auto &stmt : ast->Statements) {
    stmt->accept_vis(this);
    is_never = is_never || stmt->get_type().is_never();
  }
  if (is_never)
    return ast->set_type(Type::Never());

  // A last expression followed by `;` is a statement, its value dropped, as
  // a function body's is (FuncDefAST::body_value).
  auto *value = ast->value();
  return ast->set_type(value ? value->get_type() : Type::Unit());
}
Type BiTypeCheckerVisitor::synthesize(UnitExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();
  return ast->set_type(Type::Unit());
}
Type BiTypeCheckerVisitor::synthesize(IfExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->bool_expr->accept_vis(this);
  ast->thenBlockAST->accept_vis(this);
  ast->elseBlockAST->accept_vis(this);

  // If expression typing rule:
  // 1. Condition must be bool
  // 2. If both branches have type Never, the if has type Never
  // 3. If one branch has type Never, the if has the type of the other branch
  // 4. Otherwise, both branches must have the same type

  auto cond_type = ast->bool_expr->accept_synthesis(this);
  if (cond_type.type_kind != TypeKind::Bool) {
    this->add_error(ast->bool_expr->get_location(),
                    fmt::format("If condition must be bool, got {}",
                                cond_type.to_string()));
  }

  auto then_type = ast->thenBlockAST->accept_synthesis(this);
  auto else_type = ast->elseBlockAST->accept_synthesis(this);

  // If both branches have type Never, the if has type Never
  if (then_type.is_never() && else_type.is_never()) {
    return ast->set_type(Type::Never());
  }

  // If one branch has type Never, the if has the type of the other branch
  if (then_type.is_never()) {
    return ast->set_type(else_type);
  }
  if (else_type.is_never()) {
    return ast->set_type(then_type);
  }

  // Both branches must have compatible types (structural only — ignore
  // qualifiers)
  if (then_type != else_type) {
    if (auto common = common_type(else_type, then_type)) {
      // A branch not already of the common type flows into it.
      for (auto *block : {ast->thenBlockAST.get(), ast->elseBlockAST.get()}) {
        if (block->get_type() == *common)
          continue;
        resolve_literal_type(block->Statements.back().get(), *common);
        if (!common->is_open())
          block->set_type(*common);
      }
      return ast->set_type(*common);
    }
    this->add_error(
        ast->elseBlockAST->get_location(),
        fmt::format("If branches have incompatible types: then has {}, else "
                    "has {}",
                    then_type.to_string(), else_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  return ast->set_type(then_type);
}
Type BiTypeCheckerVisitor::synthesize(TypedVarAST *ast) {
  if (ast->synthesized())
    return ast->get_type();
  auto ast_type = resolve_type_expr(ast->type_expr.get());

  register_variable(ast->name, ast_type);
  return ast->set_type(ast_type);
}

Type BiTypeCheckerVisitor::synthesize(DerefExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->operand->accept_vis(this);

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::Pointer) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot dereference non-pointer type '{}'",
                                operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }
  // A buffer holds no one value to name: its owner says how many it holds.
  if (operand_type.is_buffer()) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot dereference {}, a buffer: index it "
                                "in an unsafe function",
                                operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  auto pointee = std::get<PointerType>(operand_type.type_data).get_pointee();

  return ast->set_type(pointee);
}

Type BiTypeCheckerVisitor::synthesize(AddrOfExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->operand->accept_vis(this);

  auto operand_type = ast->operand->accept_synthesis(this);
  if (!check_addr_of_operand(ast))
    return ast->set_type(Type::Poisoned());
  return ast->set_type(Type::Pointer(operand_type));
}

// Only a place has an address: a local binding, memory behind a pointer, or
// an element or field of a place. Reports the operand otherwise.
bool BiTypeCheckerVisitor::check_addr_of_operand(AddrOfExprAST *ast) {
  auto *root = write_root(ast->operand.get());
  if (llvm::isa<DerefExprAST, IndexExprAST>(root))
    return true;
  // A root whose type is unknown has already been reported.
  if (root->get_type().is_poisoned())
    return false;

  constexpr auto reason =
      "Only variables and memory behind a pointer have an address";
  if (auto *var = llvm::dyn_cast<VariableExprAST>(root)) {
    // A variable has a stack slot; a function or an enum variant does not.
    if (names_variable(var->variableName))
      return true;
    this->add_error(ast->get_location(),
                    fmt::format("Cannot take the address of '{}'. {}",
                                var->variableName.mangled(), reason));
    return false;
  }
  this->add_error(
      ast->get_location(),
      fmt::format("Cannot take the address of a temporary value. {}", reason));
  return false;
}

Type BiTypeCheckerVisitor::synthesize(AllocExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->operand->accept_vis(this);

  // Resolve the element type from alloc<T>
  auto element_type = resolve_type_expr(ast->type_arg.get());
  if (element_type.is_poisoned())
    return ast->set_type(Type::Poisoned());

  // The count operand must be an integer
  auto count_type = ast->operand->accept_synthesis(this);
  if (!count_type.is_integer()) {
    this->add_error(ast->operand->get_location(),
                    fmt::format("alloc count must be an integer, got '{}'",
                                count_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  if (ast->buffer)
    return ast->set_type(Type::Buffer(element_type));
  auto t = Type::Pointer(element_type);
  t.linearity = Linearity::Linear;
  return ast->set_type(t);
}

// `x as T` goes through the instance Convert<From, T>. A literal, which has
// no type of its own yet, takes T when it can, so `5000000000 as i64` keeps
// its value, and its default type (i32 or f64) otherwise. So do the literals
// inside an array or tuple.
Type BiTypeCheckerVisitor::synthesize(CastExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->operand->accept_vis(this);
  auto from = ast->operand->accept_synthesis(this);
  auto to = resolve_type_expr(ast->target_type.get());
  if (from.is_poisoned() || to.is_poisoned())
    return ast->set_type(Type::Poisoned());
  if (from.is_never())
    return ast->set_type(Type::Never());

  auto settled = from.is_polymorphic_numeric() &&
                         type_map_ordering.structurally_compatible(to, from)
                     ? to
                     : default_polymorphic_type(from);
  if (!(settled == from)) {
    resolve_literal_type(ast->operand.get(), settled);
    from = settled;
  }

  auto key = MonomorphizedKey::instance(kConvertClass, {from, to});
  if (!has_instance(key)) {
    add_error(ast->get_location(),
              missing_instance(
                  key, fmt::format("Cannot convert {} to {} with 'as': no "
                                   "instance of Convert<{}>",
                                   from.to_string(), to.to_string(),
                                   key.type_args_string())));
    return ast->set_type(Type::Poisoned());
  }
  props_.cast(ast->id()).instance = key;
  return ast->set_type(to);
}

Type BiTypeCheckerVisitor::synthesize(DropExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->operand->accept_vis(this);

  // Any value drops; one that owns nothing, doing nothing.
  ast->operand->accept_synthesis(this);
  return ast->set_type(Type::Unit());
}

Type BiTypeCheckerVisitor::synthesize(ArrayLiteralExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  for (auto &elem : ast->elements)
    elem->accept_vis(this);

  if (ast->elements.empty()) {
    this->add_error(ast->get_location(),
                    "Array literal must have at least one element");
    return ast->set_type(Type::Poisoned());
  }

  // The elements have one type, as an if's branches do: [1, 2i64] is an
  // array of i64, [None, Some(1i64)] of Option<i64>.
  auto elem_type = ast->elements[0]->accept_synthesis(this);
  for (size_t i = 1; i < ast->elements.size(); i++) {
    auto type = ast->elements[i]->accept_synthesis(this);
    if (type == elem_type)
      continue;
    auto common = common_type(elem_type, type);
    if (!common) {
      this->add_error(ast->elements[i]->get_location(),
                      fmt::format("Array element {}: expected {}, got {}", i,
                                  elem_type.to_string(), type.to_string()));
      return ast->set_type(Type::Poisoned());
    }
    elem_type = *common;
  }
  for (auto &elem : ast->elements)
    resolve_literal_type(elem.get(), elem_type);

  return ast->set_type(Type::Array(elem_type, ast->elements.size()));
}

Type BiTypeCheckerVisitor::synthesize(RangeExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->start->accept_vis(this);
  ast->end->accept_vis(this);

  auto start_type = ast->start->accept_synthesis(this);
  auto end_type = ast->end->accept_synthesis(this);

  if (start_type.is_poisoned() || end_type.is_poisoned())
    return ast->set_type(Type::Poisoned());

  // Check that both sides are integer types or polymorphic Integer
  if (!is_integer_bound(*this, ast->start.get(), start_type, "Range start") ||
      !is_integer_bound(*this, ast->end.get(), end_type, "Range end"))
    return ast->set_type(Type::Poisoned());

  // Both must be NumberExprAST (compile-time constants)
  auto *start_num = llvm::dyn_cast<NumberExprAST>(ast->start.get());
  auto *end_num = llvm::dyn_cast<NumberExprAST>(ast->end.get());
  if (!start_num || !end_num) {
    this->add_error(ast->get_location(),
                    "Range bounds must be compile-time integer constants");
    return ast->set_type(Type::Poisoned());
  }

  // Parse the literal values
  auto parsed_start = sammine_util::str_to_int<int64_t>(
      *this, start_num->number, ast->start->get_location());
  auto parsed_end = sammine_util::str_to_int<int64_t>(*this, end_num->number,
                                                      ast->end->get_location());
  if (!parsed_start || !parsed_end)
    return ast->set_type(Type::Poisoned());
  int64_t start_val = *parsed_start;
  int64_t end_val = *parsed_end;

  if (end_val < start_val) {
    this->add_error(ast->get_location(),
                    fmt::format("Range end ({}) must be >= start ({})", end_val,
                                start_val));
    return ast->set_type(Type::Poisoned());
  }

  size_t size = static_cast<size_t>(end_val - start_val + 1);

  // Resolve element type (default polymorphic Integer to i32)
  auto elem_type = default_polymorphic_type(start_type);
  resolve_literal_type(ast->start.get(), elem_type);
  resolve_literal_type(ast->end.get(), elem_type);

  return ast->set_type(Type::Array(elem_type, size));
}

Type BiTypeCheckerVisitor::synthesize(IndexExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->array_expr->accept_vis(this);
  ast->index_expr->accept_vis(this);

  auto arr_type = ast->array_expr->accept_synthesis(this);

  // Anything else that can be indexed goes through its Indexer instance.
  if (arr_type.type_kind != TypeKind::Array &&
      arr_type.type_kind != TypeKind::Pointer && !arr_type.is_poisoned())
    return ast->set_type(synthesize_indexer(ast, arr_type));

  // Validate index type (shared for both array and pointer)
  auto idx_type = ast->index_expr->accept_synthesis(this);
  if (idx_type.type_kind == TypeKind::Integer) {
    resolve_literal_type(ast->index_expr.get(), Type::I32_t());
    idx_type = Type::I32_t();
  }
  if (!idx_type.is_integer()) {
    this->add_error(ast->get_location(),
                    fmt::format("Index must be integer type, got '{}'",
                                idx_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  // Array indexing (existing path)
  if (arr_type.type_kind == TypeKind::Array) {
    auto &arr_data = std::get<ArrayType>(arr_type.type_data);
    if (auto *num = llvm::dyn_cast<NumberExprAST>(ast->index_expr.get())) {
      auto parsed_idx = sammine_util::str_to_int<int32_t>(*this, num->number,
                                                          ast->get_location());
      if (!parsed_idx)
        return ast->set_type(Type::Poisoned());
      int idx = *parsed_idx;
      int size = static_cast<int>(arr_data.get_size());
      if (idx < 0 || idx >= size) {
        this->add_error(
            ast->get_location(),
            fmt::format(
                "Array index out of bounds: index {} on array of size {}", idx,
                size));
        return ast->set_type(Type::Poisoned());
      }
    }
    return ast->set_type(arr_data.get_element());
  }

  // Pointer indexing: ptr<T>[i] -> T. Nothing checks which element `i` names
  // or that it is in bounds, so only an unsafe function may do it. A 'ptr<T>
  // holds one value: a buffer, 'buf<T>, or a view of one holds many.
  if (arr_type.type_kind == TypeKind::Pointer) {
    if (arr_type.is_linear_v() && !arr_type.is_buffer()) {
      this->add_error(ast->get_location(),
                      fmt::format("Cannot index {}: it holds one value, *p; "
                                  "a 'buf<T> holds many",
                                  arr_type.to_string()));
      return ast->set_type(Type::Poisoned());
    }
    check_unsafe_allowed(ast->get_location(), "indexing a pointer");
    auto &ptr_data = std::get<PointerType>(arr_type.type_data);
    return ast->set_type(ptr_data.get_pointee());
  }

  return ast->set_type(Type::Poisoned());
}

std::vector<MonomorphizedKey>
BiTypeCheckerVisitor::indexer_instances_for(const Type &coll) const {
  std::vector<MonomorphizedKey> found;
  auto add = [&](MonomorphizedKey key) {
    if (std::ranges::find(found, key) == found.end())
      found.push_back(std::move(key));
  };
  for (auto &key : type_class_instances)
    if (key.class_name() == kIndexerClass && key.type_args.size() == 2 &&
        key.type_args[0] == coll)
      add(key);
  if (auto it = generic_instances.find(std::string(kIndexerClass));
      it != generic_instances.end())
    for (auto &generic : it->second) {
      TypeBindings bindings;
      if (generic.types.size() != 2 ||
          !unify(generic.types[0], coll, bindings))
        continue;
      // The collection's type must decide the element's.
      auto element = substitute(generic.types[1], bindings);
      if (std::ranges::any_of(generic.ast->type_params, [&](auto &tp) {
            return contains_type_param(element, tp);
          }))
        continue;
      add(MonomorphizedKey::instance(kIndexerClass, {coll, element}));
    }
  return found;
}

// x[i], for x neither an array nor a pointer, calls the one Indexer instance
// for x's type, lending it x: Indexer<T, E>::Indexer(&x, i) -> E.
Type BiTypeCheckerVisitor::synthesize_indexer(IndexExprAST *ast,
                                              const Type &coll) {
  auto idx_type = ast->index_expr->accept_synthesis(this);
  if (idx_type.type_kind == TypeKind::Integer) {
    resolve_literal_type(ast->index_expr.get(), Type::I64_t());
    idx_type = Type::I64_t();
  }

  auto keys = indexer_instances_for(coll);
  if (keys.empty()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Cannot index type '{}' — no Indexer instance found",
                    coll.to_string()));
    return Type::Poisoned();
  }
  if (keys.size() > 1) {
    std::vector<std::string> names;
    for (auto &key : keys)
      names.push_back(
          fmt::format("{}<{}>", key.class_name(), key.type_args_string()));
    std::ranges::sort(names);
    this->add_error(ast->get_location(),
                    fmt::format("Indexing '{}' is ambiguous: {} all apply",
                                coll.to_string(), fmt::join(names, ", ")));
    return Type::Poisoned();
  }
  auto &key = keys.front();
  if (!has_instance(key)) {
    this->add_error(ast->get_location(),
                    missing_instance(key, fmt::format("No instance of {}<{}>",
                                                      key.class_name(),
                                                      key.type_args_string())));
    return Type::Poisoned();
  }
  if (!idx_type.is_poisoned() && idx_type != Type::I64_t()) {
    this->add_error(ast->index_expr->get_location(),
                    fmt::format("Indexing '{}' takes an i64 index, got '{}'",
                                coll.to_string(), idx_type.to_string()));
    return Type::Poisoned();
  }
  // The element comes back by value, and the collection still holds it: an
  // owning element would then have two owners.
  auto element = key.type_args[1];
  if (element.containsLinearTypes()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Indexing '{}' would copy out '{}', which owns memory "
                    "the collection still owns",
                    coll.to_string(), element.to_string()));
    return Type::Poisoned();
  }
  props_.index(ast->id()).indexer_method =
      key.to_typeclass_name(std::string(kIndexerClass));
  return element;
}
Type BiTypeCheckerVisitor::synthesize(LenExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->operand->accept_vis(this);

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::Array) {
    this->add_error(ast->get_location(),
                    fmt::format("len() requires array type, got '{}'",
                                operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  return ast->set_type(Type::I32_t());
}

// panic("message", args...) never gives a value: Never, which fits where
// any is expected. The message is printf's format, so it is a literal, and
// what it formats goes through dprintf's `...`: numbers, bools, chars and
// strings, which own nothing, so ending the program drops nothing of them.
Type BiTypeCheckerVisitor::synthesize(PanicExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  for (auto &arg : ast->arguments)
    arg->accept_vis(this);

  if (ast->arguments.empty()) {
    this->add_error(ast->get_location(),
                    "panic() needs a message: panic(\"what went wrong\")");
    return ast->set_type(Type::Poisoned());
  }
  bool ok = true;
  if (!llvm::isa<StringExprAST>(ast->arguments.front().get())) {
    this->add_error(ast->arguments.front()->get_location(),
                    "panic()'s message must be a string literal: it is the "
                    "format of what it prints, as printf's");
    ok = false;
  }
  for (auto &arg : llvm::drop_begin(ast->arguments)) {
    auto type = arg->accept_synthesis(this);
    if (type.is_poisoned()) {
      ok = false;
      continue;
    }
    // A string is a ptr<char> it doesn't own, as a literal is.
    bool string =
        type.type_kind == TypeKind::Pointer && !type.is_linear_v() &&
        std::get<PointerType>(type.type_data).get_pointee().type_kind ==
            TypeKind::Char;
    if (type.is_numeric() || type.type_kind == TypeKind::Bool ||
        type.type_kind == TypeKind::Char || string)
      continue;
    this->add_error(arg->get_location(),
                    fmt::format("panic() prints numbers, bools, chars and "
                                "strings, not '{}'",
                                type.to_string()));
    ok = false;
  }
  return ast->set_type(ok ? Type::Never() : Type::Poisoned());
}

Type BiTypeCheckerVisitor::synthesize(DimExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->operand->accept_vis(this);

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::Array) {
    this->add_error(ast->get_location(),
                    fmt::format("dim() requires array type, got '{}'",
                                operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  // Walk nested array types to collect all dimensions
  std::vector<Type> dim_types;
  Type current = operand_type;
  while (current.type_kind == TypeKind::Array) {
    auto &arr = std::get<ArrayType>(current.type_data);
    dim_types.push_back(Type::I32_t());
    current = arr.get_element();
  }

  return ast->set_type(Type::Tuple(std::move(dim_types)));
}

Type BiTypeCheckerVisitor::synthesize(UnaryNegExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->operand->accept_vis(this);

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.is_unsigned()) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot negate unsigned type '{}'",
                                operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }
  if (!operand_type.is_numeric()) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot negate non-numeric type '{}'",
                                operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }
  return ast->set_type(operand_type);
}

Type BiTypeCheckerVisitor::synthesize(StructLiteralExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  for (auto &val : ast->field_values)
    val->accept_vis(this);

  auto type_opt = get_typename_type(ast->struct_name);

  // If not found and we have explicit type args, try generic struct
  // instantiation
  if (!type_opt.has_value() && !ast->explicit_type_args.empty()) {
    auto base_name = ast->struct_name.mangled();
    auto *generic_def = monomorphizer.generic_structs.find(ast->struct_name);
    if (generic_def) {

      if (ast->explicit_type_args.size() != generic_def->type_params.size()) {
        this->add_error(
            ast->get_location(),
            fmt::format(
                "Generic struct '{}' expects {} type argument(s), got {}",
                base_name, generic_def->type_params.size(),
                ast->explicit_type_args.size()));
        return ast->set_type(Type::Poisoned());
      }

      auto resolved = resolve_explicit_type_args(ast->explicit_type_args,
                                                 generic_def->type_params);
      if (resolved) {
        auto &[bindings, type_args] = *resolved;
        MonomorphizedKey key{generic_def->struct_name, type_args};
        monomorphizer.instantiate_struct(generic_def, key, bindings)
            ->accept_vis(this);
        type_opt = get_typename_type(key.generic_name());
      }
    }
  }

  if (!type_opt.has_value()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Unknown struct type '{}'", ast->struct_name.mangled()));
    return ast->set_type(Type::Poisoned());
  }

  auto struct_type = type_opt.value();
  if (struct_type.type_kind != TypeKind::Struct) {
    this->add_error(
        ast->get_location(),
        fmt::format("'{}' is not a struct type", ast->struct_name.mangled()));
    return ast->set_type(Type::Poisoned());
  }

  auto &st = std::get<StructType>(struct_type.type_data);

  // Check field count
  if (ast->field_names.size() != st.field_count()) {
    this->add_error(ast->get_location(),
                    fmt::format("Struct '{}' expects {} fields, got {}",
                                ast->struct_name.mangled(), st.field_count(),
                                ast->field_names.size()));
    return ast->set_type(Type::Poisoned());
  }

  // Check each field
  for (size_t i = 0; i < ast->field_names.size(); i++) {
    auto idx = st.get_field_index(ast->field_names[i]);
    if (!idx.has_value()) {
      this->add_error(ast->field_values[i]->get_location(),
                      fmt::format("Struct '{}' has no field named '{}'",
                                  ast->struct_name.mangled(),
                                  ast->field_names[i]));
      return ast->set_type(Type::Poisoned());
    }

    auto expected = st.get_field_type(idx.value());
    auto actual = ast->field_values[i]->accept_synthesis(this);
    if (!type_map_ordering.compatible_to_from(expected, actual)) {
      report_type_mismatch(ast->field_values[i]->get_location(),
                           fmt::format("Field '{}': expected type {}, got {}",
                                       ast->field_names[i],
                                       expected.to_string(),
                                       actual.to_string()),
                           expected, actual);
      return ast->set_type(Type::Poisoned());
    }
    resolve_literal_type(ast->field_values[i].get(), expected);
  }

  return ast->set_type(struct_type);
}

Type BiTypeCheckerVisitor::synthesize(FieldAccessExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->object_expr->accept_vis(this);

  auto obj_type = ast->object_expr->accept_synthesis(this);
  if (obj_type.is_poisoned())
    return ast->set_type(Type::Poisoned());

  // Tuple indexing: t.0, t.1, ...
  if (obj_type.type_kind == TypeKind::Tuple) {
    auto &tup = std::get<TupleType>(obj_type.type_data);
    size_t idx;
    try {
      idx = std::stoul(ast->field_name);
    } catch (...) {
      this->add_error(ast->get_location(),
                      fmt::format("Tuple index must be a number, got '{}'",
                                  ast->field_name));
      return ast->set_type(Type::Poisoned());
    }
    if (idx >= tup.size()) {
      this->add_error(
          ast->get_location(),
          fmt::format("Tuple index {} out of bounds for tuple of size {}",
                      idx, tup.size()));
      return ast->set_type(Type::Poisoned());
    }
    return ast->set_type(tup.get_element(idx));
  }

  if (obj_type.type_kind != TypeKind::Struct) {
    this->add_error(
        ast->get_location(),
        fmt::format("Cannot access field '{}' on non-struct type {}",
                    ast->field_name, obj_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  auto &st = std::get<StructType>(obj_type.type_data);
  auto idx = st.get_field_index(ast->field_name);
  if (!idx.has_value()) {
    this->add_error(ast->get_location(),
                    fmt::format("Struct '{}' has no field named '{}'",
                                st.get_name().mangled(), ast->field_name));
    return ast->set_type(Type::Poisoned());
  }

  return ast->set_type(st.get_field_type(idx.value()));
}

// sammine has no while loop: with immutable bindings its condition could only
// change through memory behind a pointer. The parser still builds one so it
// can be reported here, and so a later pass can suggest the `for` loop or
// recursion it should be.
Type BiTypeCheckerVisitor::synthesize(WhileExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();
  this->add_error(ast->get_location(), "sammine has no 'while' loop: use "
                                       "'for i in a..b' or recursion");
  return ast->set_type(Type::Poisoned());
}

Type BiTypeCheckerVisitor::synthesize(ForExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->start->accept_vis(this);
  ast->end->accept_vis(this);

  // The loop variable lives in a scope around the body.
  ScopeGuard scope(*this);

  auto start_type = ast->start->accept_synthesis(this);
  auto end_type = ast->end->accept_synthesis(this);

  if (start_type.type_kind == TypeKind::Poisoned ||
      end_type.type_kind == TypeKind::Poisoned)
    return ast->set_type(Type::Poisoned());

  if (!is_integer_bound(*this, ast->start.get(), start_type,
                        "for-range start") ||
      !is_integer_bound(*this, ast->end.get(), end_type, "for-range end"))
    return ast->set_type(Type::Poisoned());

  // The bounds have one type, as an if's branches do: a literal bound takes
  // the other's, as in 1..n with n an i64, and two literals default to i32.
  auto common = type_map_ordering.lowest_common_type(start_type, end_type);
  auto resolved = default_polymorphic_type(common ? *common : start_type);

  if (!type_map_ordering.compatible_to_from(resolved, start_type) ||
      !type_map_ordering.compatible_to_from(resolved, end_type)) {
    this->add_error(
        ast->get_location(),
        fmt::format("for-range start and end must be the same type, got {} and {}",
                    start_type.to_string(), end_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  resolve_literal_type(ast->start.get(), resolved);
  resolve_literal_type(ast->end.get(), resolved);

  // Verify that Iterator<Range<element_type>, element_type> instance exists.
  // Declaring that instance is what instantiates Range<element_type>.
  auto range_type = get_typename_type(bare("Range").with_type_args({resolved}));
  if (!range_type || !has_instance(MonomorphizedKey::instance(
                         "Iterator", {*range_type, resolved}))) {
    this->add_error(
        ast->get_location(),
        fmt::format("No Iterator instance for Range with element type {}",
                    resolved.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  props_.for_loop(ast->id()).range_type = *range_type;

  // Compile-time empty range check
  auto *start_num = llvm::dyn_cast<NumberExprAST>(ast->start.get());
  auto *end_num = llvm::dyn_cast<NumberExprAST>(ast->end.get());
  if (start_num && end_num) {
    bool nonempty =
        resolved.type_kind == TypeKind::U64_t
            ? is_nonempty_for_range<uint64_t>(*this, ast, start_num, end_num)
            : is_nonempty_for_range<int64_t>(*this, ast, start_num, end_num);
    if (!nonempty)
      return ast->set_type(Type::Poisoned());
  }

  register_variable(ast->loop_var, resolved);

  ast->set_type(Type::Unit());
  ast->body->accept_vis(this);
  return ast->get_type();
}

Type BiTypeCheckerVisitor::synthesize(TupleLiteralExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  for (auto &elem : ast->elements)
    elem->accept_vis(this);
  std::vector<Type> elem_types;
  for (auto &elem : ast->elements) {
    auto t = elem->accept_synthesis(this);
    if (t.is_poisoned())
      return ast->set_type(Type::Poisoned());
    elem_types.push_back(t);
  }
  return ast->set_type(Type::Tuple(std::move(elem_types)));
}

Type BiTypeCheckerVisitor::synthesize(TypeClassDeclAST *ast) {
  return Type::NonExistent();
}
Type BiTypeCheckerVisitor::synthesize(TypeClassInstanceAST *ast) {
  return Type::NonExistent();
}
Type BiTypeCheckerVisitor::synthesize(KernelDefAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  // Prototype was already visited by visit(KernelDefAST*), so param types
  // are registered in id_to_type and the prototype has a Function type.
  auto proto_type = ast->Prototype->get_type();
  if (proto_type.is_poisoned())
    return ast->set_type(Type::Poisoned());

  auto &fn_type = std::get<FunctionType>(proto_type.type_data);
  auto declared_ret = fn_type.get_return_type();

  // A kernel computes on numbers: it takes and returns scalars, or arrays of
  // them, and may return nothing.
  auto scalar = [](const Type &type) {
    switch (type.type_kind) {
    case TypeKind::I32_t:
    case TypeKind::I64_t:
    case TypeKind::U32_t:
    case TypeKind::U64_t:
    case TypeKind::F64_t:
    case TypeKind::F32_t:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Integer:
    case TypeKind::Flt:
      return true;
    default:
      return false;
    }
  };
  auto computable = [&](const Type &type) {
    if (type.type_kind == TypeKind::Array)
      return scalar(std::get<ArrayType>(type.type_data).get_element());
    return scalar(type);
  };
  bool computes = true;
  for (auto &param : ast->Prototype->parameterVectors)
    if (!computable(param->get_type())) {
      this->add_error(
          param->get_location(),
          fmt::format("A kernel takes only numbers, bools and chars, or "
                      "arrays of them; '{}' is '{}'",
                      param->name, param->get_type().to_string()));
      computes = false;
    }
  if (declared_ret.type_kind != TypeKind::Unit && !computable(declared_ret)) {
    this->add_error(
        ast->Prototype->return_type_expr->location,
        fmt::format("A kernel returns only numbers, bools and chars, or "
                    "arrays of them, not '{}'",
                    declared_ret.to_string()));
    computes = false;
  }
  if (!computes)
    return ast->set_type(Type::Poisoned());

  // Type-check each kernel body expression
  Type body_result_type = Type::Unit();
  for (auto &expr : ast->Body->expressions) {
    if (auto *map_expr = llvm::dyn_cast<KernelMapExprAST>(expr.get())) {
      body_result_type = synthesize_kernel_map(ast, map_expr);
    } else if (auto *reduce_expr =
                   llvm::dyn_cast<KernelReduceExprAST>(expr.get())) {
      body_result_type = synthesize_kernel_reduce(ast, reduce_expr);
    } else if (auto *num = llvm::dyn_cast<KernelNumberExprAST>(expr.get())) {
      // Number literal — assume declared return type
      if (!is_valid_number_lexeme(*this, num->number, declared_ret,
                                  num->get_location()))
        return ast->set_type(Type::Poisoned());
      body_result_type = declared_ret;
    } else {
      this->add_error(expr->get_location(),
                      "Unsupported expression in kernel body; "
                      "only map, reduce, and literals are allowed");
      return ast->set_type(Type::Poisoned());
    }

    if (body_result_type.is_poisoned())
      return ast->set_type(Type::Poisoned());

    expr->set_type(body_result_type);
  }

  // Verify body result type matches declared return type
  if (!type_map_ordering.compatible_to_from(declared_ret, body_result_type)) {
    if (body_result_type.is_polymorphic_numeric() &&
        type_map_ordering.structurally_compatible(declared_ret,
                                                  body_result_type)) {
      body_result_type = declared_ret;
    } else {
      this->add_error(
          ast->get_location(),
          fmt::format(
              "Kernel body type '{}' does not match declared return type '{}'",
              body_result_type.to_string(), declared_ret.to_string()));
      return ast->set_type(Type::Poisoned());
    }
  }

  return ast->set_type(proto_type);
}

std::optional<Type>
BiTypeCheckerVisitor::lookup_kernel_input_array(const std::string &name,
                                                const char *op,
                                                sammine_util::Location loc) {
  auto input_type = lookup_type(bare(name));
  if (!input_type) {
    this->add_error(loc,
                    fmt::format("Unknown variable '{}' in kernel {}", name, op));
    return std::nullopt;
  }

  if (input_type->type_kind != TypeKind::Array) {
    this->add_error(loc,
                    fmt::format("Kernel {} input '{}' must be an array type, "
                                "got '{}'",
                                op, name, input_type->to_string()));
    return std::nullopt;
  }
  return input_type;
}

Type BiTypeCheckerVisitor::synthesize_kernel_map(KernelDefAST *kd,
                                                 KernelMapExprAST *map_expr) {
  // 1-2. Look up input array in scope and verify it's an Array type
  auto input_type = lookup_kernel_input_array(map_expr->input_name, "map",
                                              map_expr->get_location());
  if (!input_type)
    return Type::Poisoned();

  auto &arr_type = std::get<ArrayType>(input_type->type_data);
  auto elem_type = arr_type.get_element();
  auto arr_size = arr_type.get_size();

  // 3. Check lambda has exactly 1 parameter
  if (map_expr->lambda_proto->parameterVectors.size() != 1) {
    this->add_error(
        map_expr->lambda_proto->get_location(),
        fmt::format("Kernel map lambda must have exactly 1 parameter, got {}",
                    map_expr->lambda_proto->parameterVectors.size()));
    return Type::Poisoned();
  }

  // 4. Push new scope for lambda body and synthesize parameter
  ScopeGuard scope(*this);

  map_expr->lambda_proto->parameterVectors[0]->accept_synthesis(this);
  auto param_type = map_expr->lambda_proto->parameterVectors[0]->get_type();

  // 5. Check param type matches array element type
  if (!type_map_ordering.compatible_to_from(param_type, elem_type)) {
    this->add_error(
        map_expr->lambda_proto->parameterVectors[0]->get_location(),
        fmt::format("Kernel map lambda parameter type '{}' does not match "
                    "array element type '{}'",
                    param_type.to_string(), elem_type.to_string()));
    return Type::Poisoned();
  }

  // 6. Resolve declared return type of the lambda
  auto declared_ret =
      resolve_type_expr(map_expr->lambda_proto->return_type_expr.get());
  if (declared_ret.is_poisoned())
    return Type::Poisoned();

  // 7. Visit the lambda body (walks children via accept_vis, then synthesizes)
  auto body_type = map_expr->lambda_body->accept_synthesis(this);

  if (body_type.is_poisoned())
    return Type::Poisoned();

  // 8. Handle polymorphic numeric literals in the body
  if (body_type.is_polymorphic_numeric() &&
      type_map_ordering.structurally_compatible(declared_ret, body_type)) {
    if (!map_expr->lambda_body->Statements.empty()) {
      resolve_literal_type(map_expr->lambda_body->Statements.back().get(),
                           declared_ret);
    }
    body_type = declared_ret;
    map_expr->lambda_body->set_type(declared_ret);
  }

  // 9. Check body type matches declared return type of lambda
  if (!type_map_ordering.compatible_to_from(declared_ret, body_type)) {
    this->add_error(
        map_expr->lambda_body->get_location(),
        fmt::format("Kernel map lambda body type '{}' does not match "
                    "declared return type '{}'",
                    body_type.to_string(), declared_ret.to_string()));
    return Type::Poisoned();
  }

  // 10. Result is an array of the lambda return type with same size
  return Type::Array(declared_ret, arr_size);
}

Type BiTypeCheckerVisitor::synthesize_kernel_reduce(
    KernelDefAST *kd, KernelReduceExprAST *reduce_expr) {
  // 1-2. Look up input array in scope and verify it's an Array type
  auto input_type = lookup_kernel_input_array(reduce_expr->input_name, "reduce",
                                              reduce_expr->get_location());
  if (!input_type)
    return Type::Poisoned();

  auto &arr_type = std::get<ArrayType>(input_type->type_data);
  auto elem_type = arr_type.get_element();

  // 3. Verify operator is valid (+, -, *, /)
  auto op_kind = reduce_expr->op_tok->tok_type;
  if (op_kind != TokADD && op_kind != TokMUL && op_kind != TokSUB &&
      op_kind != TokDIV) {
    this->add_error(
        reduce_expr->op_tok->get_location(),
        fmt::format("Kernel reduce operator must be +, -, *, or /; got '{}'",
                    reduce_expr->op_tok->lexeme));
    return Type::Poisoned();
  }

  // 4. Verify element type is numeric (concrete, not polymorphic)
  if (!elem_type.is_numeric() || elem_type.is_polymorphic_numeric()) {
    this->add_error(
        reduce_expr->get_location(),
        fmt::format("Kernel reduce requires a numeric array element type, "
                    "got '{}'",
                    elem_type.to_string()));
    return Type::Poisoned();
  }

  // 5. Synthesize identity expression
  auto identity_type = reduce_expr->identity->accept_synthesis(this);
  if (identity_type.is_poisoned())
    return Type::Poisoned();

  // 6. Resolve polymorphic numeric literals to the element type
  if (identity_type.is_polymorphic_numeric()) {
    if (type_map_ordering.structurally_compatible(elem_type, identity_type)) {
      resolve_literal_type(reduce_expr->identity.get(), elem_type);
      identity_type = elem_type;
    } else {
      identity_type = default_polymorphic_type(identity_type);
      resolve_literal_type(reduce_expr->identity.get(), identity_type);
    }
  }

  // 7. Check identity type is compatible with element type
  if (!type_map_ordering.compatible_to_from(elem_type, identity_type)) {
    this->add_error(
        reduce_expr->identity->get_location(),
        fmt::format("Kernel reduce identity type '{}' is not compatible "
                    "with array element type '{}'",
                    identity_type.to_string(), elem_type.to_string()));
    return Type::Poisoned();
  }

  // 8. Reduce collapses the array to its element type
  return elem_type;
}

Type BiTypeCheckerVisitor::synthesize(CaseExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  ast->scrutinee->accept_vis(this);

  auto scrutinee_type = ast->scrutinee->accept_synthesis(this);
  if (scrutinee_type.is_poisoned())
    return ast->set_type(Type::Poisoned());
  // Its variants are matched by its type, which nothing else decides.
  if (scrutinee_type.is_open()) {
    report_open_type(ast->scrutinee.get());
    return ast->set_type(Type::Poisoned());
  }

  // Detect mode: literal vs enum
  bool has_literal = false, has_variant = false;
  for (auto &arm : ast->arms) {
    if (arm.pattern.is_wildcard)
      continue;
    if (arm.pattern.is_literal)
      has_literal = true;
    else
      has_variant = true;
  }

  if (has_literal && has_variant) {
    this->add_error(
        ast->get_location(),
        "Cannot mix literal and enum variant patterns in case expression");
    return ast->set_type(Type::Poisoned());
  }

  auto patterns = has_literal ? check_literal_patterns(ast, scrutinee_type)
                              : check_enum_patterns(ast, scrutinee_type);
  if (patterns == CasePatternResult::Fatal)
    return ast->set_type(Type::Poisoned());

  bool had_error = patterns == CasePatternResult::InvalidArms;
  auto result_type = check_arm_bodies(ast, scrutinee_type, had_error);
  if (had_error)
    return ast->set_type(Type::Poisoned());

  return ast->set_type(result_type);
}

// Validates literal patterns and their exhaustiveness. Polymorphic scrutinees
// are resolved to their default type, so `scrutinee_type` may be updated.
CasePatternResult
BiTypeCheckerVisitor::check_literal_patterns(CaseExprAST *ast,
                                             Type &scrutinee_type) {
  bool had_error = false;

  // Resolve polymorphic scrutinee (e.g. Integer → i32)
  if (scrutinee_type.is_polymorphic_numeric()) {
    scrutinee_type = default_polymorphic_type(scrutinee_type);
    resolve_literal_type(ast->scrutinee.get(), scrutinee_type);
  }

  // Reject float scrutinees
  if (scrutinee_type.is_float()) {
    this->add_error(
        ast->scrutinee->get_location(),
        fmt::format("Float types cannot be used in literal case "
                    "patterns (float equality is unreliable), got {}",
                    scrutinee_type.to_string()));
    return CasePatternResult::Fatal;
  }

  // Must be integer, bool, or char
  if (!scrutinee_type.is_matchable_scalar()) {
    this->add_error(
        ast->scrutinee->get_location(),
        fmt::format("Literal case patterns require an integer, bool, or char "
                    "scrutinee type, got {}",
                    scrutinee_type.to_string()));
    return CasePatternResult::Fatal;
  }

  // Validate each literal pattern
  using LK = CasePattern::LiteralKind;
  std::set<std::string> seen_literals;
  for (auto &arm : ast->arms) {
    if (arm.pattern.is_wildcard || !arm.pattern.is_literal)
      continue;

    // Check for duplicates
    if (!seen_literals.insert(arm.pattern.literal_value).second) {
      this->add_error(arm.pattern.location,
                      fmt::format("Duplicate literal pattern '{}'",
                                  arm.pattern.literal_value));
      had_error = true;
      continue;
    }

    if (arm.pattern.literal_kind == LK::Integer &&
        !is_valid_number_lexeme(*this, arm.pattern.literal_value,
                                scrutinee_type, arm.pattern.location)) {
      had_error = true;
      continue;
    }

    // Validate literal kind matches scrutinee type
    if (scrutinee_type.type_kind == TypeKind::Bool) {
      if (arm.pattern.literal_kind != LK::Bool) {
        this->add_error(arm.pattern.location,
                        fmt::format("Expected boolean pattern, got '{}'",
                                    arm.pattern.literal_value));
        had_error = true;
      }
    } else if (scrutinee_type.type_kind == TypeKind::Char) {
      if (arm.pattern.literal_kind != LK::Char) {
        this->add_error(arm.pattern.location,
                        fmt::format("Expected character pattern, got '{}'",
                                    arm.pattern.literal_value));
        had_error = true;
      }
    } else {
      // Integer scrutinee
      if (arm.pattern.literal_kind != LK::Integer) {
        this->add_error(
            arm.pattern.location,
            fmt::format("Expected integer pattern for {} scrutinee, got '{}'",
                        scrutinee_type.to_string(),
                        arm.pattern.literal_value));
        had_error = true;
      }
    }
  }

  // Exhaustiveness for literals
  if (!had_error) {
    bool has_wildcard =
        std::any_of(ast->arms.begin(), ast->arms.end(),
                    [](const auto &a) { return a.pattern.is_wildcard; });

    if (scrutinee_type.type_kind == TypeKind::Bool) {
      // Bool: exhaustive if both true and false covered, or wildcard present
      if (!has_wildcard) {
        bool has_true = seen_literals.contains("true");
        bool has_false = seen_literals.contains("false");
        if (!has_true || !has_false) {
          std::string missing;
          if (!has_true)
            missing += "true";
          if (!has_false) {
            if (!missing.empty())
              missing += ", ";
            missing += "false";
          }
          this->add_error(ast->scrutinee->get_location(),
                          fmt::format("Non-exhaustive case expression: "
                                      "missing pattern(s) {}",
                                      missing));
          had_error = true;
        }
      }
    } else {
      // Integer/char: wildcard always required
      if (!has_wildcard) {
        this->add_error(ast->scrutinee->get_location(),
                        fmt::format("Non-exhaustive case expression on {}: "
                                    "a wildcard '_' pattern is required",
                                    scrutinee_type.to_string()));
        had_error = true;
      }
    }
  }

  return had_error ? CasePatternResult::InvalidArms : CasePatternResult::Valid;
}

// Validates enum patterns, resolves their variant indices, and checks
// exhaustiveness.
CasePatternResult
BiTypeCheckerVisitor::check_enum_patterns(CaseExprAST *ast,
                                          const Type &scrutinee_type) {
  bool had_error = false;

  if (scrutinee_type.type_kind != TypeKind::Enum) {
    this->add_error(
        ast->scrutinee->get_location(),
        fmt::format("Case expression requires an enum type, got {}",
                    scrutinee_type.to_string()));
    return CasePatternResult::Fatal;
  }

  auto &et = std::get<EnumType>(scrutinee_type.type_data);

  for (auto &arm : ast->arms) {
    if (arm.pattern.is_wildcard)
      continue;

    auto variant_name = arm.pattern.variant_name.get_name();
    auto variant_idx = et.get_variant_index(variant_name);
    if (!variant_idx.has_value()) {
      this->add_error(arm.pattern.location,
                      fmt::format("Type '{}' has no variant '{}'",
                                  et.get_name().mangled(), variant_name));
      had_error = true;
      continue;
    }

    arm.pattern.variant_index = variant_idx.value();
    auto &vi = et.get_variant(variant_idx.value());

    if (arm.pattern.bindings.size() != vi.payload_types.size()) {
      this->add_error(
          arm.pattern.location,
          fmt::format("Pattern '{}::{}' expects {} bindings, got {}",
                      et.get_name().mangled(), variant_name,
                      vi.payload_types.size(), arm.pattern.bindings.size()));
      had_error = true;
      continue;
    }
  }

  // Enum exhaustiveness check
  if (!had_error) {
    bool has_wildcard = false;
    std::set<size_t> covered_indices;
    for (auto &arm : ast->arms) {
      if (arm.pattern.is_wildcard) {
        has_wildcard = true;
        break;
      }
      covered_indices.insert(arm.pattern.variant_index);
    }

    if (!has_wildcard && covered_indices.size() < et.variant_count()) {
      std::string missing;
      for (size_t i = 0; i < et.variant_count(); i++) {
        if (!covered_indices.contains(i)) {
          if (!missing.empty())
            missing += ", ";
          missing += et.get_variant(i).name;
        }
      }
      this->add_error(
          ast->scrutinee->get_location(),
          fmt::format("Non-exhaustive case expression: missing variant(s) {}",
                      missing));
      had_error = true;
    }
  }

  return had_error ? CasePatternResult::InvalidArms : CasePatternResult::Valid;
}

// Type-checks each arm body in its own scope and unifies their types.
// Sets `had_error` if the arms have incompatible types.
Type BiTypeCheckerVisitor::check_arm_bodies(CaseExprAST *ast,
                                            const Type &scrutinee_type,
                                            bool &had_error) {
  // ASTContext is inherited on enter_new_scope(), so enclosing_function
  // is automatically available inside arm scopes — no manual capture needed.
  Type result_type = Type::Never();

  for (auto &arm : ast->arms) {
    if (had_error && !arm.pattern.is_wildcard && !arm.pattern.is_literal)
      continue; // skip arms with validation errors

    ScopeGuard scope(*this);

    // Register enum payload bindings (only for non-wildcard, non-literal enum
    // arms)
    if (!arm.pattern.is_wildcard && !arm.pattern.is_literal &&
        scrutinee_type.type_kind == TypeKind::Enum) {
      auto &et = std::get<EnumType>(scrutinee_type.type_data);
      auto &vi = et.get_variant(arm.pattern.variant_index);
      for (size_t i = 0; i < arm.pattern.bindings.size(); i++) {
        register_variable(arm.pattern.bindings[i], vi.payload_types[i]);
      }
    }

    auto arm_type = arm.body->accept_synthesis(this);

    // Unify with result type
    if (result_type.is_never()) {
      result_type = arm_type;
    } else if (!arm_type.is_never() && arm_type != result_type) {
      if (auto common = common_type(result_type, arm_type)) {
        result_type = *common;
      } else {
        report_type_mismatch(
            arm.body->get_location(),
            fmt::format(
                "Case arms have incompatible types: expected {}, got {}",
                result_type.to_string(), arm_type.to_string()),
            result_type, arm_type);
        had_error = true;
      }
    }
  }

  if (had_error)
    return result_type;

  // Arms holding polymorphic literals take the unified result type.
  for (auto &arm : ast->arms) {
    if (arm.body->get_type().is_polymorphic() &&
        !arm.body->Statements.empty()) {
      resolve_literal_type(arm.body->Statements.back().get(), result_type);
      arm.body->set_type(result_type);
    }
  }

  return result_type;
}

// --- Generic support: unification, substitution ---

} // namespace sammine_lang::AST
