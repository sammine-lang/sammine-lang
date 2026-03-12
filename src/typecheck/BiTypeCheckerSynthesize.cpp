#include "typecheck/BiTypeChecker.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "typecheck/Types.h"
#include "typecheck/UniSubst.h"
#include "util/MonomorphizedName.h"

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
    if (i > 0) result += ", ";
    auto it = bindings.find(type_params[i]);
    result += (it != bindings.end()) ? it->second.to_string() : type_params[i];
  }
  return result + ">";
}

std::optional<std::pair<TypeBindings, std::string>>
BiTypeCheckerVisitor::resolve_explicit_type_args(
    const std::vector<std::unique_ptr<TypeExprAST>> &explicit_type_args,
    const std::vector<std::string> &type_params) {
  TypeBindings bindings;
  std::string type_args_str;
  for (size_t i = 0; i < explicit_type_args.size(); i++) {
    auto resolved = resolve_type_expr(explicit_type_args[i].get());
    if (resolved.type_kind == TypeKind::Poisoned)
      return std::nullopt;
    bindings[type_params[i]] = resolved;
    if (i > 0) type_args_str += ", ";
    type_args_str += resolved.to_string();
  }
  return std::make_pair(std::move(bindings), std::move(type_args_str));
}

Type BiTypeCheckerVisitor::synthesize(ProgramAST *ast) {
  return Type::NonExistent();
}

// Variable definition: infers type from expression, checks against annotation if present.
// Polymorphic literals (Integer/Flt) resolve to the annotated type via compatible_to_from.
Type BiTypeCheckerVisitor::synthesize(VarDefAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  // Handle tuple destructuring: let (a, b) = expr;
  if (ast->is_tuple_destructure) {
    if (!ast->Expression) {
      this->add_error(ast->get_location(),
                      "Destructuring requires an initializer expression");
      return ast->set_type(Type::Poisoned());
    }
    auto expr_type = ast->Expression->accept_synthesis(this);
    if (expr_type.is_poisoned())
      return ast->set_type(Type::Poisoned());

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

    for (size_t i = 0; i < tt.size(); i++) {
      auto elem_type = tt.get_element(i);
      // If the var has a type annotation, check compatibility
      if (ast->destructure_vars[i]->type_expr) {
        auto ann_type = ast->destructure_vars[i]->accept_synthesis(this);
        if (!type_map_ordering.compatible_to_from(ann_type, elem_type)) {
          this->add_error(
              ast->destructure_vars[i]->get_location(),
              fmt::format("Type annotation '{}' incompatible with tuple "
                          "element type '{}'",
                          ann_type.to_string(), elem_type.to_string()));
          if (auto hint = incompatibility_hint(ann_type, elem_type))
            this->add_diagnostics(ast->destructure_vars[i]->get_location(),
                                  *hint);
          return ast->set_type(Type::Poisoned());
        }
        elem_type = ann_type;
      }
      ast->destructure_vars[i]->set_type(elem_type);
      id_to_type.registerNameT(ast->destructure_vars[i]->name, elem_type);
    }

    return ast->set_type(Type::Unit());
  }

  // if you dont have type lexeme for typed var, then just assign type of expr
  // to typed var, if we dont have expr also, then we add error
  //
  // if you do, then just use type lexeme as type of typed var

  if (ast->TypedVar->type_expr != nullptr)
    ast->set_type(ast->TypedVar->accept_synthesis(this));
  else if (ast->Expression) {
    ast->set_type(ast->Expression->accept_synthesis(this));
    // No annotation: default polymorphic literals (Integer→i32, Flt→f64)
    if (ast->get_type().is_polymorphic_numeric()) {
      auto concrete = default_polymorphic_type(ast->get_type());
      resolve_literal_type(ast->Expression.get(), concrete);
      ast->set_type(concrete);
    }
  } else {
    this->add_error(ast->get_location(),
                    "Variable declared without initializer");
    ast->set_type(Type::Poisoned());
  }

  { auto t = ast->get_type(); t.mutability = ast->is_mutable ? Mutability::Mutable : Mutability::Immutable; ast->set_type(t); }

  id_to_type.registerNameT(ast->TypedVar->name, ast->get_type());
  LOG({
    fmt::print(stderr, "[typecheck] synthesize VarDefAST: '{}' : {} ({}{})\n",
               ast->TypedVar->name, ast->get_type().to_string(),
               ast->is_mutable ? "mutable" : "immutable",
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

// Call resolution order: enum variant → generic function → typeclass method → normal call.
// If fewer args than params, creates a partial application (closure).
Type BiTypeCheckerVisitor::synthesize(CallExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  // INFO: All enum variant calls arrive here in qualified form (Enum::Variant).
  // The scope generator rewrites unqualified variant names (e.g. Some(42)) to
  // qualified form (e.g. Option::Some(42)) via variant_to_enum. The type checker
  // does NOT resolve unqualified enum variant names — that is the scope
  // generator's responsibility.
  if (ast->functionName.is_qualified()) {
    auto enum_type_opt = get_typename_type(ast->functionName.get_qualifier());

    // If not found and we have explicit type args, try generic enum instantiation
    if (!enum_type_opt && !ast->explicit_type_args.empty()) {
      // Extract base name (everything before '<')
      auto module = ast->functionName.get_qualifier();
      auto angle_pos = module.find('<');
      if (angle_pos != std::string::npos) {
        std::string base_name = module.substr(0, angle_pos);
        auto *generic_def = monomorphizer.find_generic_enum(base_name);
        if (generic_def) {
          if (ast->explicit_type_args.size() == generic_def->type_params.size()) {
            auto resolved = resolve_explicit_type_args(
                ast->explicit_type_args, generic_def->type_params);
            if (resolved) {
              auto &[bindings, type_args_str] = *resolved;
              auto mono = sammine_util::MonomorphizedName::generic(
                  generic_def->enum_name, "<" + type_args_str + ">");
              monomorphizer.instantiate_enum(generic_def, mono, bindings)
                  ->accept_vis(this);
              enum_type_opt = get_typename_type(mono.mangled());
            }
          }
        }
      }
    }

    // Enum variant without explicit type args — use fully-qualified mangled name
    // (e.g., Some(42) rewritten to Option::Some by scope generator)
    if (!enum_type_opt) {
      auto vc_it = variant_constructors.find(ast->functionName.mangled());
      if (vc_it != variant_constructors.end())
        enum_type_opt = vc_it->second.first;
    }

    if (enum_type_opt && enum_type_opt->type_kind == TypeKind::Enum) {
      auto &enum_type = *enum_type_opt;
      auto &et = std::get<EnumType>(enum_type.type_data);
      auto variant_idx = et.get_variant_index(ast->functionName.get_name());
      if (!variant_idx) {
        this->add_error(
            ast->get_location(),
            fmt::format("Type '{}' has no variant '{}'",
                        ast->functionName.get_qualifier(), ast->functionName.get_name()));
        return ast->set_type(Type::Poisoned());
      }
      auto &vi = et.get_variant(*variant_idx);

      if (ast->arguments.size() != vi.payload_types.size()) {
        this->add_error(
            ast->get_location(),
            fmt::format("Enum variant '{}::{}' expects {} arguments, got {}",
                        ast->functionName.get_qualifier(), vi.name,
                        vi.payload_types.size(), ast->arguments.size()));
        return ast->set_type(Type::Poisoned());
      }

      for (size_t i = 0; i < ast->arguments.size(); i++) {
        auto arg_type = ast->arguments[i]->accept_synthesis(this);
        if (!type_map_ordering.compatible_to_from(vi.payload_types[i],
                                                  arg_type)) {
          this->add_error(
              ast->arguments[i]->get_location(),
              fmt::format("Type mismatch in enum variant '{}::{}' argument {}: "
                          "expected {}, got {}",
                          ast->functionName.get_qualifier(), vi.name, i + 1,
                          vi.payload_types[i].to_string(),
                          arg_type.to_string()));
          if (auto hint =
                  incompatibility_hint(vi.payload_types[i], arg_type))
            this->add_diagnostics(ast->arguments[i]->get_location(), *hint);
          return ast->set_type(Type::Poisoned());
        }
      }

      props_.call(ast->id()).is_enum_constructor = true;
      props_.call(ast->id()).enum_variant_index = *variant_idx;
      return ast->set_type(enum_type);
    }
  }

  // Try typeclass dispatch first (requires explicit type args)
  if (!ast->explicit_type_args.empty()) {
    auto result = synthesize_typeclass_call(ast);
    if (result.has_value())
      return *result;
  }

  // Try generic function instantiation
  if (monomorphizer.find_generic_func(ast->functionName.mangled()))
    return synthesize_generic_call(ast);

  // Normal function call
  return synthesize_normal_call(ast);
}

std::optional<Type>
BiTypeCheckerVisitor::synthesize_typeclass_call(CallExprAST *ast) {
  // Support both syntaxes:
  //   add<i32>(x, y)       — method_name = "add", type arg in explicit_type_args
  //   Add<i32>::add(x, y)  — method_name = "add" (get_name()), class from qualifier
  std::string method_name;
  std::string class_name;

  if (ast->functionName.is_qualified()) {
    // Qualified syntax: Add<i32>::add — extract method from last part
    method_name = ast->functionName.get_name();
    auto qualifier = ast->functionName.get_qualifier();
    auto angle_pos = qualifier.find('<');
    if (angle_pos != std::string::npos)
      class_name = qualifier.substr(0, angle_pos);
  }

  if (class_name.empty()) {
    // Unqualified syntax: add<i32>(x, y) — look up method in method_to_class
    method_name = ast->functionName.mangled();
    auto class_it = method_to_class.find(method_name);
    if (class_it == method_to_class.end())
      return std::nullopt;
    class_name = class_it->second;
  }

  auto tc_it = type_class_defs.find(class_name);
  if (tc_it == type_class_defs.end())
    return std::nullopt;
  auto &tc = tc_it->second;

  if (ast->explicit_type_args.size() != tc.type_params.size()) {
    add_error(ast->get_location(),
              fmt::format("Type class '{}' expects {} type argument(s), got {}",
                          class_name, tc.type_params.size(),
                          ast->explicit_type_args.size()));
    return ast->set_type(Type::Poisoned());
  }

  auto resolved_args = resolve_explicit_type_args(
      ast->explicit_type_args, tc.type_params);
  if (!resolved_args)
    return ast->set_type(Type::Poisoned());
  auto &[tc_bindings, concrete_types_str] = *resolved_args;

  std::vector<Type> tc_type_args;
  for (auto &tp : tc.type_params)
    tc_type_args.push_back(tc_bindings.at(tp));
  auto inst_it = type_class_instances.find(
      TypeClassKey{class_name, tc_type_args});
  if (inst_it == type_class_instances.end()) {
    add_error(ast->get_location(),
              fmt::format("No instance of {}<{}>", class_name,
                          concrete_types_str));
    return ast->set_type(Type::Poisoned());
  }

  PrototypeAST *method_proto = nullptr;
  for (auto *p : tc.methods) {
    if (p->functionName.mangled() == method_name) {
      method_proto = p;
      break;
    }
  }
  if (!method_proto) {
    add_error(ast->get_location(),
              fmt::format("Method '{}' not found in type class '{}'",
                          method_name, class_name));
    return ast->set_type(Type::Poisoned());
  }

  auto func_type = std::get<FunctionType>(method_proto->get_type().type_data);
  auto params = func_type.get_params_types();
  if (ast->arguments.size() != params.size()) {
    add_error(ast->get_location(),
              fmt::format("Type class method '{}' expects {} arguments, got {}",
                          method_name, params.size(), ast->arguments.size()));
    return ast->set_type(Type::Poisoned());
  }

  props_.call(ast->id()).resolved_name =
      inst_it->second.method_mangled_names[method_name];
  props_.call(ast->id()).is_typeclass_call = true;

  auto return_type = func_type.get_return_type();
  return ast->set_type(substitute(return_type, tc_bindings));
}

Type BiTypeCheckerVisitor::synthesize_generic_call(CallExprAST *ast) {
  auto *generic_def = monomorphizer.find_generic_func(ast->functionName.mangled());
  auto generic_type = generic_def->get_type();
  auto func = std::get<FunctionType>(generic_type.type_data);
  auto params = func.get_params_types();

  if (ast->arguments.size() != params.size()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Generic function '{}' expects {} arguments, got {}",
                    ast->functionName.mangled(), params.size(),
                    ast->arguments.size()));
    return ast->set_type(Type::Poisoned());
  }

  TypeBindings bindings;

  if (!ast->explicit_type_args.empty()) {
    // Explicit type arguments: f<T, U>(args) — must provide all
    auto &type_params = generic_def->Prototype->type_params;
    if (ast->explicit_type_args.size() != type_params.size()) {
      this->add_error(
          ast->get_location(),
          fmt::format("Expected {} type argument(s) for '{}', got {}",
                      type_params.size(), ast->functionName.mangled(),
                      ast->explicit_type_args.size()));
      return ast->set_type(Type::Poisoned());
    }

    auto resolved_args = resolve_explicit_type_args(
        ast->explicit_type_args, type_params);
    if (!resolved_args)
      return ast->set_type(Type::Poisoned());
    bindings = std::move(resolved_args->first);

    // Register concrete bindings in a temporary scope so resolve_type_expr
    // can resolve generic struct types (e.g. Vec<T> → Vec<i32>).
    {
      decltype(typename_to_type)::Guard scope(typename_to_type);
      for (auto &[pname, ptype] : bindings)
        typename_to_type.registerNameT(pname, ptype);

      for (size_t i = 0; i < ast->arguments.size(); i++) {
        auto arg_type = ast->arguments[i]->accept_synthesis(this);
        if (arg_type.type_kind == TypeKind::Poisoned) {
          return ast->set_type(Type::Poisoned());
        }
        // Resolve expected type from the parameter AST with concrete bindings,
        // rather than substitute() on the possibly-placeholder stored type.
        auto *param_type_ast =
            generic_def->Prototype->parameterVectors[i]->type_expr.get();
        auto expected = param_type_ast ? resolve_type_expr(param_type_ast)
                                       : substitute(params[i], bindings);
        if (!type_map_ordering.compatible_to_from(expected, arg_type)) {
          auto call_name = format_generic_call_name(
              ast->functionName.with_alias(), type_params, bindings);
          this->add_error(
              ast->arguments[i]->get_location(),
              fmt::format("Argument {} to '{}': expected {}, got {}", i + 1,
                          call_name, expected.to_string(),
                          arg_type.to_string()));
          if (auto hint = incompatibility_hint(expected, arg_type))
            this->add_diagnostics(ast->arguments[i]->get_location(), *hint);
          auto mono_sig = substitute(generic_type, bindings);
          this->add_diagnostics(
              ast->arguments[i]->get_location(),
              fmt::format("note: '{}' has signature: {}",
                          call_name, mono_sig.to_string()));
          return ast->set_type(Type::Poisoned());
        }
        if (arg_type.is_polymorphic_numeric()) {
          resolve_literal_type(ast->arguments[i].get(), expected);
        }
      }
    }
  } else {
    // Infer type arguments from call arguments
    for (size_t i = 0; i < ast->arguments.size(); i++) {
      auto arg_type = ast->arguments[i]->accept_synthesis(this);
      if (arg_type.type_kind == TypeKind::Poisoned)
        return ast->set_type(Type::Poisoned());
      // Default polymorphic literals before generic unification
      if (arg_type.is_polymorphic_numeric()) {
        arg_type = default_polymorphic_type(arg_type);
        resolve_literal_type(ast->arguments[i].get(), arg_type);
      }
      if (!unify(params[i], arg_type, bindings)) {
        auto call_name = format_generic_call_name(
            ast->functionName.with_alias(),
            generic_def->Prototype->type_params, bindings);
        auto expected = substitute(params[i], bindings);
        this->add_error(ast->arguments[i]->get_location(),
                        fmt::format("Type mismatch in argument {} of '{}': "
                                    "expected {}, got {}",
                                    i + 1, call_name,
                                    expected.to_string(),
                                    arg_type.to_string()));
        auto mono_sig = substitute(generic_type, bindings);
        this->add_diagnostics(
            ast->arguments[i]->get_location(),
            fmt::format("note: '{}' has signature: {}",
                        call_name, mono_sig.to_string()));
        return ast->set_type(Type::Poisoned());
      }
    }

    // Check all type params resolved
    for (auto &tp : generic_def->Prototype->type_params) {
      if (bindings.find(tp) == bindings.end()) {
        this->add_error(
            ast->get_location(),
            fmt::format(
                "Type parameter '{}' could not be inferred for '{}'",
                tp, ast->functionName.mangled()));
        return ast->set_type(Type::Poisoned());
      }
    }
  }

  std::string type_args = "<";
  for (size_t i = 0; i < generic_def->Prototype->type_params.size(); i++) {
    if (i > 0) type_args += ", ";
    type_args += bindings[generic_def->Prototype->type_params[i]].to_string();
  }
  type_args += ">";

  props_.call(ast->id()).resolved_name =
      sammine_util::MonomorphizedName::generic(ast->functionName, type_args);
  props_.call(ast->id()).type_bindings = bindings;
  props_.call(ast->id()).callee_func_type = generic_type;

  // Resolve return type by re-evaluating the return type AST with concrete
  // bindings registered, rather than using substitute(). This ensures that
  // generic struct return types (e.g. Box<T> → Box<i32>) get properly
  // instantiated through the normal resolve_type_expr path.
  auto *ret_type_ast = generic_def->Prototype->return_type_expr.get();
  if (ret_type_ast) {
    decltype(typename_to_type)::Guard scope(typename_to_type);
    for (auto &[pname, ptype] : bindings)
      typename_to_type.registerNameT(pname, ptype);
    auto resolved_ret = resolve_type_expr(ret_type_ast);
    return ast->set_type(resolved_ret);
  }
  return ast->set_type(substitute(func.get_return_type(), bindings));
}

Type BiTypeCheckerVisitor::synthesize_normal_call(CallExprAST *ast) {
  auto ty = try_get_callee_type(ast->functionName.mangled());
  if (!ty.has_value()) {
    this->add_error(ast->get_location(),
                    fmt::format("Function '{}' not found",
                                ast->functionName.mangled()));
    return ast->set_type(Type::Poisoned());
  }

  props_.call(ast->id()).callee_func_type = ty;

  if (ty->type_kind != TypeKind::Function) {
    this->add_error(ast->get_location(),
                    fmt::format("'{}' is not callable",
                                ast->functionName.mangled()));
    return ast->set_type(Type::Poisoned());
  }

  auto func = std::get<FunctionType>(ty->type_data);
  auto params = func.get_params_types();

  // Variadic functions: accept any number of args >= fixed params
  if (func.is_var_arg()) {
    if (ast->arguments.size() < params.size()) {
      this->add_error(
          ast->get_location(),
          fmt::format("Variadic function '{}' requires at least {} "
                      "arguments, got {}",
                      ast->functionName.mangled(), params.size(),
                      ast->arguments.size()));
      return ast->set_type(Type::Poisoned());
    }
    return ast->set_type(func.get_return_type());
  }

  if (ast->arguments.size() > params.size()) {
    this->add_error(ast->get_location(),
                    fmt::format("Function '{}' expects {} arguments, got {}",
                                ast->functionName.mangled(), params.size(),
                                ast->arguments.size()));
    return ast->set_type(Type::Poisoned());
  }

  // Partial application: fewer args than params
  if (ast->arguments.size() < params.size()) {
    props_.call(ast->id()).is_partial = true;
    std::vector<Type> remaining;
    for (size_t i = ast->arguments.size(); i < params.size(); i++)
      remaining.push_back(params[i]);
    remaining.push_back(func.get_return_type());
    return ast->set_type(Type::Function(std::move(remaining)));
  }

  // Arg type-checking happens in visit(CallExprAST*) after args are visited,
  // since synthesize runs before args have their types set.
  return ast->set_type(func.get_return_type());
}

Type BiTypeCheckerVisitor::synthesize(ReturnStmtAST *ast) {
  return ast->set_type(Type::Never());
}
// Binary ops: resolved via typeclass instances (e.g. Add<i32>::add for `+`).
// Comparison ops return bool. Assignment checks mutability. Logical ops require bool.
Type BiTypeCheckerVisitor::synthesize(BinaryExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  if (ast->LHS->get_type().type_kind == TypeKind::Poisoned ||
      ast->RHS->get_type().type_kind == TypeKind::Poisoned)
    return ast->set_type(Type::Poisoned());

  if (ast->LHS->get_type().type_kind == TypeKind::Never ||
      ast->RHS->get_type().type_kind == TypeKind::Never)
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
  if (ast->LHS->get_type().is_polymorphic_numeric() &&
      !ast->RHS->get_type().is_polymorphic_numeric()) {
    if (type_map_ordering.structurally_compatible(ast->RHS->get_type(), ast->LHS->get_type()))
      resolve_literal_type(ast->LHS.get(), ast->RHS->get_type());
  } else if (ast->RHS->get_type().is_polymorphic_numeric() &&
             !ast->LHS->get_type().is_polymorphic_numeric()) {
    if (type_map_ordering.structurally_compatible(ast->LHS->get_type(), ast->RHS->get_type()))
      resolve_literal_type(ast->RHS.get(), ast->LHS->get_type());
  }

  if (!this->type_map_ordering.compatible_to_from(ast->LHS->get_type(),
                                                  ast->RHS->get_type())) {
    this->add_error(
        ast->Op->get_location(),
        fmt::format("Incompatible types for operator '{}': {} and {}",
                    ast->Op->lexeme, ast->LHS->get_type().to_string(),
                    ast->RHS->get_type().to_string()));
    if (auto hint =
            incompatibility_hint(ast->LHS->get_type(), ast->RHS->get_type()))
      this->add_diagnostics(ast->Op->get_location(), *hint);
    return ast->set_type(Type::Poisoned());
  }

  return synthesize_binary_operator(ast, ast->LHS->get_type(), ast->RHS->get_type());
}

Type BiTypeCheckerVisitor::synthesize_binary_operator(BinaryExprAST *ast,
                                                      const Type &lhs_type,
                                                      const Type &rhs_type) {
  if (ast->Op->is_comparison()) {
    auto kind = lhs_type.type_kind;
    if (kind == TypeKind::Array || kind == TypeKind::Pointer) {
      if (ast->Op->tok_type != TokEQUAL && ast->Op->tok_type != TokNOTEqual) {
        this->add_error(
            ast->Op->get_location(),
            fmt::format("Only == and != are supported for {} types",
                        lhs_type.to_string()));
        return ast->set_type(Type::Poisoned());
      }
    }
    return ast->set_type(Type::Bool());
  }

  if (ast->Op->is_assign()) {
    if (auto *var = llvm::dyn_cast<VariableExprAST>(ast->LHS.get())) {
      if (!var->get_type().is_mutable_v()) {
        this->add_error(
            ast->Op->get_location(),
            fmt::format("Cannot reassign immutable variable '{}'. "
                        "Use 'let mut' or 'mut' to declare it as mutable",
                        var->variableName));
        return ast->set_type(Type::Poisoned());
      }
    } else if (auto *idx = llvm::dyn_cast<IndexExprAST>(ast->LHS.get())) {
      if (auto *arr_var =
              llvm::dyn_cast<VariableExprAST>(idx->array_expr.get())) {
        if (!arr_var->get_type().is_mutable_v()) {
          this->add_error(
              ast->Op->get_location(),
              fmt::format(
                  "Cannot write to index of immutable array '{}'. "
                  "Use 'let mut' to declare it as mutable",
                  arr_var->variableName));
          return ast->set_type(Type::Poisoned());
        }
      }
    }
    return ast->set_type(Type::Unit());
  }

  if (ast->Op->is_logical())
    return ast->set_type(lhs_type);

  // Bitwise operators: valid on integer types and integer-backed enums
  if (ast->Op->is_bitwise()) {
    bool valid = lhs_type.type_kind == TypeKind::I32_t ||
                 lhs_type.type_kind == TypeKind::I64_t ||
                 lhs_type.type_kind == TypeKind::U32_t ||
                 lhs_type.type_kind == TypeKind::U64_t ||
                 lhs_type.type_kind == TypeKind::Integer;
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

  // Arithmetic operators dispatch through typeclasses
  static const std::unordered_map<int, std::pair<std::string, std::string>>
      op_to_class = {
          {TokADD, {"Add", "Add"}}, {TokSUB, {"Sub", "Sub"}},
          {TokMUL, {"Mul", "Mul"}}, {TokDIV, {"Div", "Div"}},
          {TokMOD, {"Mod", "Mod"}},
      };

  auto it = op_to_class.find(ast->Op->tok_type);
  if (it != op_to_class.end()) {
    auto &[class_name, method_name] = it->second;
    auto inst_it = type_class_instances.find(
        TypeClassKey{class_name, {lhs_type}});
    if (inst_it == type_class_instances.end()) {
      add_error(ast->Op->get_location(),
                fmt::format("No instance of {}<{}> — cannot use '{}' on this "
                            "type",
                            class_name, lhs_type.to_string(),
                            ast->Op->lexeme));
      return ast->set_type(Type::Poisoned());
    }
    auto method_it = inst_it->second.method_mangled_names.find(method_name);
    if (method_it != inst_it->second.method_mangled_names.end())
      props_.binary(ast->id()).resolved_op_method = method_it->second;
    return ast->set_type(lhs_type);
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

  this->abort_on(ast->number.empty(),
                 "NumberExprAST should have a number lexeme");

  // Check for type suffix (e.g., 42i64, 3.14f64)
  // Find the first alpha character — everything from there is the suffix
  std::string suffix;
  size_t suffix_start = ast->number.size();
  for (size_t i = 0; i < ast->number.size(); ++i) {
    if (std::isalpha(ast->number[i])) {
      suffix_start = i;
      break;
    }
  }
  if (suffix_start < ast->number.size()) {
    suffix = ast->number.substr(suffix_start);
    ast->number = ast->number.substr(0, suffix_start);
  }

  if (!suffix.empty()) {
    if (suffix == "i32")
      ast->set_type(Type::I32_t());
    else if (suffix == "i64")
      ast->set_type(Type::I64_t());
    else if (suffix == "u32")
      ast->set_type(Type::U32_t());
    else if (suffix == "u64")
      ast->set_type(Type::U64_t());
    else if (suffix == "f64")
      ast->set_type(Type::F64_t());
    else if (suffix == "f32")
      ast->set_type(Type::F32_t());
    else
      this->abort_on(
          true,
          fmt::format("invalid type suffix '{}' on number literal", suffix));
  } else if (ast->number.find('.') == std::string::npos)
    ast->set_type(Type::Integer());
  else
    ast->set_type(Type::Flt());

  return ast->get_type();
}
Type BiTypeCheckerVisitor::synthesize(BoolExprAST *ast) {
  return ast->set_type(Type::Bool());
}
Type BiTypeCheckerVisitor::synthesize(CharExprAST *ast) {
  return ast->set_type(Type::Char());
}
Type BiTypeCheckerVisitor::synthesize(VariableExprAST *ast) {
  // Check if the name exists in type scope before looking it up
  // (recursive_get_from_name aborts on not-found)
  if (id_to_type.recursiveQueryName(ast->variableName) == nameFound) {
    ast->set_type(id_to_type.recursive_get_from_name(ast->variableName));
  } else {
    // Try zero-payload enum variant (e.g., None, Red)
    auto it = variant_constructors.find(ast->variableName);
    if (it != variant_constructors.end()) {
      auto &[enum_type, variant_idx] = it->second;
      auto &et = std::get<EnumType>(enum_type.type_data);
      auto &vi = et.get_variant(variant_idx);
      if (vi.payload_types.empty()) {
        props_.variable(ast->id()).is_enum_unit_variant = true;
        props_.variable(ast->id()).enum_variant_index = variant_idx;
        return ast->set_type(enum_type);
      }
    }
    // Not found anywhere — use the original abort path for proper error
    ast->set_type(id_to_type.recursive_get_from_name(ast->variableName));
  }
  LOG({
    fmt::print(stderr, "[typecheck] synthesize VariableExprAST: '{}' -> {}\n",
               ast->variableName, ast->get_type().to_string());
  });
  return ast->get_type();
}
Type BiTypeCheckerVisitor::synthesize(BlockAST *ast) {
  // Block typing rule:
  // 1. Type each statement in order
  // 2. If any statement has type ! (Never) and is not the return , stop: the
  // block's type is !
  // 3. Otherwise, the block's type is the type of the last expression
  // 4. If there is no final expression, the type is ()

  if (ast->Statements.empty()) {
    return ast->set_type(Type::Unit());
  }

  for (auto &stmt : ast->Statements) {
    auto stmt_type = stmt->accept_synthesis(this);
    if (stmt_type.type_kind == TypeKind::Never) {
      return ast->set_type(Type::Never());
    }
  }

  // Block's type is the type of the last expression
  return ast->set_type(ast->Statements.back()->get_type());
}
Type BiTypeCheckerVisitor::synthesize(UnitExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();
  return ast->set_type(Type::Unit());
}
Type BiTypeCheckerVisitor::synthesize(IfExprAST *ast) {
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
  if (then_type.type_kind == TypeKind::Never &&
      else_type.type_kind == TypeKind::Never) {
    return ast->set_type(Type::Never());
  }

  // If one branch has type Never, the if has the type of the other branch
  if (then_type.type_kind == TypeKind::Never) {
    return ast->set_type(else_type);
  }
  if (else_type.type_kind == TypeKind::Never) {
    return ast->set_type(then_type);
  }

  // Both branches must have compatible types (structural only — ignore qualifiers)
  if (then_type != else_type) {
    if (type_map_ordering.structurally_compatible(else_type, then_type)) {
      resolve_literal_type(ast->thenBlockAST->Statements.back().get(),
                           else_type);
      ast->thenBlockAST->set_type(else_type);
      return ast->set_type(else_type);
    } else if (type_map_ordering.structurally_compatible(then_type, else_type)) {
      resolve_literal_type(ast->elseBlockAST->Statements.back().get(),
                           then_type);
      ast->elseBlockAST->set_type(then_type);
      return ast->set_type(then_type);
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

  ast_type.mutability = ast->is_mutable ? Mutability::Mutable : Mutability::Immutable;
  id_to_type.registerNameT(ast->name, ast_type);
  return ast->set_type(ast_type);
}

Type BiTypeCheckerVisitor::synthesize(DerefExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::Pointer) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot dereference non-pointer type '{}'",
                                operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  auto pointee = std::get<PointerType>(operand_type.type_data).get_pointee();

  return ast->set_type(pointee);
}

Type BiTypeCheckerVisitor::synthesize(AddrOfExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  auto operand_type = ast->operand->accept_synthesis(this);
  return ast->set_type(Type::Pointer(operand_type));
}

Type BiTypeCheckerVisitor::synthesize(AllocExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  // Resolve the element type from alloc<T>
  auto element_type = resolve_type_expr(ast->type_arg.get());
  if (element_type.type_kind == TypeKind::Poisoned)
    return ast->set_type(Type::Poisoned());

  // The count operand must be an integer
  auto count_type = ast->operand->accept_synthesis(this);
  if (count_type.type_kind != TypeKind::I32_t &&
      count_type.type_kind != TypeKind::I64_t &&
      count_type.type_kind != TypeKind::U32_t &&
      count_type.type_kind != TypeKind::U64_t &&
      count_type.type_kind != TypeKind::Integer) {
    this->add_error(ast->operand->get_location(),
                    fmt::format("alloc count must be an integer, got '{}'",
                                count_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  { auto t = Type::Pointer(element_type); t.linearity = Linearity::Linear; ast->set_type(t); } // alloc always produces linear pointers
  return ast->get_type();
}

Type BiTypeCheckerVisitor::synthesize(FreeExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::Pointer) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot free non-pointer type '{}'",
                                operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }
  return ast->set_type(Type::Unit());
}

Type BiTypeCheckerVisitor::synthesize(ArrayLiteralExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  if (ast->elements.empty()) {
    this->add_error(ast->get_location(),
                    "Array literal must have at least one element");
    return ast->set_type(Type::Poisoned());
  }

  auto first_type = ast->elements[0]->accept_synthesis(this);
  for (size_t i = 1; i < ast->elements.size(); i++) {
    auto elem_type = ast->elements[i]->accept_synthesis(this);
    if (elem_type != first_type) {
      this->add_error(ast->elements[i]->get_location(),
                      fmt::format("Array element {}: expected {}, got {}", i,
                                  first_type.to_string(),
                                  elem_type.to_string()));
      return ast->set_type(Type::Poisoned());
    }
  }

  // Default polymorphic element type (annotation resolution happens in visit(VarDefAST*))
  if (first_type.is_polymorphic_numeric()) {
    auto concrete = default_polymorphic_type(first_type);
    for (auto &elem : ast->elements)
      resolve_literal_type(elem.get(), concrete);
    first_type = concrete;
  }

  return ast->set_type(Type::Array(first_type, ast->elements.size()));
}

Type BiTypeCheckerVisitor::synthesize(RangeExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  auto start_type = ast->start->accept_synthesis(this);
  auto end_type = ast->end->accept_synthesis(this);

  // Check that both sides are integer types or polymorphic Integer
  // Valid TypeKinds: I32_t, I64_t, U32_t, U64_t, Integer (polymorphic)
  auto is_int = [](const Type &t) {
    return t.type_kind == TypeKind::I32_t || t.type_kind == TypeKind::I64_t ||
           t.type_kind == TypeKind::U32_t || t.type_kind == TypeKind::U64_t ||
           t.type_kind == TypeKind::Integer;
  };

  if (!is_int(start_type)) {
    this->add_error(ast->start->get_location(),
                    fmt::format("Range start must be an integer type, got {}",
                                start_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }
  if (!is_int(end_type)) {
    this->add_error(ast->end->get_location(),
                    fmt::format("Range end must be an integer type, got {}",
                                end_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  // Both must be NumberExprAST (compile-time constants)
  auto *start_num = llvm::dyn_cast<NumberExprAST>(ast->start.get());
  auto *end_num = llvm::dyn_cast<NumberExprAST>(ast->end.get());
  if (!start_num || !end_num) {
    this->add_error(ast->get_location(),
                    "Range bounds must be compile-time integer constants");
    return ast->set_type(Type::Poisoned());
  }

  // Parse the literal values
  int64_t start_val = std::stoll(start_num->number);
  int64_t end_val = std::stoll(end_num->number);

  if (end_val < start_val) {
    this->add_error(ast->get_location(),
                    fmt::format("Range end ({}) must be >= start ({})",
                                end_val, start_val));
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

  auto arr_type = ast->array_expr->accept_synthesis(this);

  // Validate index type (shared for both array and pointer)
  auto idx_type = ast->index_expr->accept_synthesis(this);
  if (idx_type.type_kind == TypeKind::Integer) {
    resolve_literal_type(ast->index_expr.get(), Type::I32_t());
    idx_type = Type::I32_t();
  }
  if (idx_type.type_kind != TypeKind::I32_t &&
      idx_type.type_kind != TypeKind::I64_t &&
      idx_type.type_kind != TypeKind::U32_t &&
      idx_type.type_kind != TypeKind::U64_t) {
    this->add_error(ast->get_location(),
                    fmt::format("Index must be integer type, got '{}'",
                                idx_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  // Array indexing (existing path)
  if (arr_type.type_kind == TypeKind::Array) {
    auto &arr_data = std::get<ArrayType>(arr_type.type_data);
    if (auto *num = llvm::dyn_cast<NumberExprAST>(ast->index_expr.get())) {
      int idx = std::stoi(num->number);
      int size = static_cast<int>(arr_data.get_size());
      if (idx < 0 || idx >= size) {
        this->add_error(
            ast->get_location(),
            fmt::format(
                "Array index out of bounds: index {} on array of size {}",
                idx, size));
        return ast->set_type(Type::Poisoned());
      }
    }
    return ast->set_type(arr_data.get_element());
  }

  // Pointer indexing: ptr<T>[i] -> T
  if (arr_type.type_kind == TypeKind::Pointer) {
    auto &ptr_data = std::get<PointerType>(arr_type.type_data);
    return ast->set_type(ptr_data.get_pointee());
  }

  this->add_error(
      ast->get_location(),
      fmt::format("Cannot index type '{}' — no Indexer instance found",
                  arr_type.to_string()));
  return ast->set_type(Type::Poisoned());
}
Type BiTypeCheckerVisitor::synthesize(LenExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::Array) {
    this->add_error(ast->get_location(),
                    fmt::format("len() requires array type, got '{}'",
                                operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  return ast->set_type(Type::I32_t());
}

Type BiTypeCheckerVisitor::synthesize(DimExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

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

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind == TypeKind::U32_t ||
      operand_type.type_kind == TypeKind::U64_t) {
    this->add_error(
        ast->get_location(),
        fmt::format("Cannot negate unsigned type '{}'",
                    operand_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }
  if (operand_type.type_kind != TypeKind::I32_t &&
      operand_type.type_kind != TypeKind::I64_t &&
      operand_type.type_kind != TypeKind::F64_t &&
      operand_type.type_kind != TypeKind::F32_t &&
      operand_type.type_kind != TypeKind::Integer &&
      operand_type.type_kind != TypeKind::Flt) {
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

  if (ast->struct_name.is_unresolved()) {
    this->add_error(ast->get_location(),
                    fmt::format("Module '{}' is not imported",
                                ast->struct_name.get_module()));
    return ast->set_type(Type::Poisoned());
  }

  auto type_opt = get_typename_type(ast->struct_name.mangled());

  // If not found and we have explicit type args, try generic struct instantiation
  if (!type_opt.has_value() && !ast->explicit_type_args.empty()) {
    auto base_name = ast->struct_name.mangled();
    auto *generic_def = monomorphizer.find_generic_struct(base_name);
    if (generic_def) {

      if (ast->explicit_type_args.size() != generic_def->type_params.size()) {
        this->add_error(
            ast->get_location(),
            fmt::format("Generic struct '{}' expects {} type argument(s), got {}",
                        base_name, generic_def->type_params.size(),
                        ast->explicit_type_args.size()));
        return ast->set_type(Type::Poisoned());
      }

      TypeBindings bindings;
      std::string type_args_str = "<";
      bool ok = true;
      for (size_t i = 0; i < ast->explicit_type_args.size(); i++) {
        auto resolved = resolve_type_expr(ast->explicit_type_args[i].get());
        if (resolved.is_poisoned()) { ok = false; break; }
        bindings[generic_def->type_params[i]] = resolved;
        if (i > 0) type_args_str += ", ";
        type_args_str += resolved.to_string();
      }
      type_args_str += ">";

      auto mono = sammine_util::MonomorphizedName::generic(
          generic_def->struct_name, type_args_str);
      auto mangled = mono.mangled();

      if (ok) {
        monomorphizer.instantiate_struct(generic_def, mono, bindings)
            ->accept_vis(this);
        type_opt = get_typename_type(mangled);
      }
    }
  }

  if (!type_opt.has_value()) {
    this->add_error(ast->get_location(),
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
    this->add_error(
        ast->get_location(),
        fmt::format("Struct '{}' expects {} fields, got {}", ast->struct_name.mangled(),
                    st.field_count(), ast->field_names.size()));
    return ast->set_type(Type::Poisoned());
  }

  // Check each field
  for (size_t i = 0; i < ast->field_names.size(); i++) {
    auto idx = st.get_field_index(ast->field_names[i]);
    if (!idx.has_value()) {
      this->add_error(
          ast->field_values[i]->get_location(),
          fmt::format("Struct '{}' has no field named '{}'", ast->struct_name.mangled(),
                      ast->field_names[i]));
      return ast->set_type(Type::Poisoned());
    }

    auto expected = st.get_field_type(idx.value());
    auto actual = ast->field_values[i]->accept_synthesis(this);
    if (!type_map_ordering.compatible_to_from(expected, actual)) {
      this->add_error(
          ast->field_values[i]->get_location(),
          fmt::format("Field '{}': expected type {}, got {}",
                      ast->field_names[i], expected.to_string(),
                      actual.to_string()));
      if (auto hint = incompatibility_hint(expected, actual))
        this->add_diagnostics(ast->field_values[i]->get_location(), *hint);
      return ast->set_type(Type::Poisoned());
    }
  }

  return ast->set_type(struct_type);
}

Type BiTypeCheckerVisitor::synthesize(FieldAccessExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  auto obj_type = ast->object_expr->accept_synthesis(this);
  if (obj_type.type_kind == TypeKind::Poisoned)
    return ast->set_type(Type::Poisoned());

  if (obj_type.type_kind != TypeKind::Struct) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot access field '{}' on non-struct type {}",
                                ast->field_name, obj_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  auto &st = std::get<StructType>(obj_type.type_data);
  auto idx = st.get_field_index(ast->field_name);
  if (!idx.has_value()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Struct '{}' has no field named '{}'",
                    st.get_name().mangled(), ast->field_name));
    return ast->set_type(Type::Poisoned());
  }

  return ast->set_type(st.get_field_type(idx.value()));
}

Type BiTypeCheckerVisitor::synthesize(WhileExprAST *ast) {
  if (!ast->condition || !ast->body)
    return ast->set_type(Type::Poisoned());
  auto cond_type = ast->condition->accept_synthesis(this);
  if (cond_type != Type::Bool() && cond_type != Type::Poisoned()) {
    this->add_error(ast->condition->get_location(),
                    fmt::format("while condition must be bool, found {}",
                                cond_type.to_string()));
  }
  return ast->set_type(Type::Unit());
}

Type BiTypeCheckerVisitor::synthesize(TupleLiteralExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();
  std::vector<Type> elem_types;
  for (auto &elem : ast->elements) {
    auto t = elem->accept_synthesis(this);
    if (t.is_poisoned())
      return ast->set_type(Type::Poisoned());
    // Default polymorphic literals in tuple elements
    if (t.is_polymorphic_numeric()) {
      auto concrete = default_polymorphic_type(t);
      resolve_literal_type(elem.get(), concrete);
      t = concrete;
    }
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

  // Type-check each kernel body expression
  Type body_result_type = Type::Unit();
  for (auto &expr : ast->Body->expressions) {
    if (auto *map_expr = llvm::dyn_cast<KernelMapExprAST>(expr.get())) {
      body_result_type = synthesize_kernel_map(ast, map_expr);
    } else if (auto *reduce_expr =
                   llvm::dyn_cast<KernelReduceExprAST>(expr.get())) {
      body_result_type = synthesize_kernel_reduce(ast, reduce_expr);
    } else if (llvm::isa<KernelNumberExprAST>(expr.get())) {
      // Number literal — assume declared return type
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

Type BiTypeCheckerVisitor::synthesize_kernel_map(KernelDefAST *kd,
                                                 KernelMapExprAST *map_expr) {
  // 1. Look up input array in scope
  if (id_to_type.top().recursiveQueryName(map_expr->input_name) == nameNotFound) {
    this->add_error(map_expr->get_location(),
                    fmt::format("Unknown variable '{}' in kernel map",
                                map_expr->input_name));
    return Type::Poisoned();
  }
  auto input_type =
      id_to_type.top().recursive_get_from_name(map_expr->input_name);

  // 2. Verify it's an Array type
  if (input_type.type_kind != TypeKind::Array) {
    this->add_error(
        map_expr->get_location(),
        fmt::format("Kernel map input '{}' must be an array type, got '{}'",
                    map_expr->input_name, input_type.to_string()));
    return Type::Poisoned();
  }

  auto &arr_type = std::get<ArrayType>(input_type.type_data);
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
  enter_new_scope();

  map_expr->lambda_proto->parameterVectors[0]->accept_synthesis(this);
  auto param_type = map_expr->lambda_proto->parameterVectors[0]->get_type();

  // 5. Check param type matches array element type
  if (!type_map_ordering.compatible_to_from(param_type, elem_type)) {
    this->add_error(
        map_expr->lambda_proto->parameterVectors[0]->get_location(),
        fmt::format(
            "Kernel map lambda parameter type '{}' does not match "
            "array element type '{}'",
            param_type.to_string(), elem_type.to_string()));
    exit_new_scope();
    return Type::Poisoned();
  }

  // 6. Resolve declared return type of the lambda
  auto declared_ret =
      resolve_type_expr(map_expr->lambda_proto->return_type_expr.get());
  if (declared_ret.is_poisoned()) {
    exit_new_scope();
    return Type::Poisoned();
  }

  // 7. Visit the lambda body (walks children via accept_vis, then synthesizes)
  map_expr->lambda_body->accept_vis(this);
  auto body_type = map_expr->lambda_body->accept_synthesis(this);

  exit_new_scope();

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
        fmt::format(
            "Kernel map lambda body type '{}' does not match "
            "declared return type '{}'",
            body_type.to_string(), declared_ret.to_string()));
    return Type::Poisoned();
  }

  // 10. Result is an array of the lambda return type with same size
  return Type::Array(declared_ret, arr_size);
}

Type BiTypeCheckerVisitor::synthesize_kernel_reduce(
    KernelDefAST *kd, KernelReduceExprAST *reduce_expr) {
  // 1. Look up input array in scope
  if (id_to_type.top().recursiveQueryName(reduce_expr->input_name) ==
      nameNotFound) {
    this->add_error(reduce_expr->get_location(),
                    fmt::format("Unknown variable '{}' in kernel reduce",
                                reduce_expr->input_name));
    return Type::Poisoned();
  }
  auto input_type =
      id_to_type.top().recursive_get_from_name(reduce_expr->input_name);

  // 2. Verify Array type and extract element type
  if (input_type.type_kind != TypeKind::Array) {
    this->add_error(
        reduce_expr->get_location(),
        fmt::format(
            "Kernel reduce input '{}' must be an array type, got '{}'",
            reduce_expr->input_name, input_type.to_string()));
    return Type::Poisoned();
  }

  auto &arr_type = std::get<ArrayType>(input_type.type_data);
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

  // 4. Verify element type is numeric
  switch (elem_type.type_kind) {
  case TypeKind::I32_t:
  case TypeKind::I64_t:
  case TypeKind::U32_t:
  case TypeKind::U64_t:
  case TypeKind::F64_t:
  case TypeKind::F32_t:
    break;
  default:
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

  // 1. Synthesize scrutinee
  auto scrutinee_type = ast->scrutinee->accept_synthesis(this);
  if (scrutinee_type.type_kind == TypeKind::Poisoned)
    return ast->set_type(Type::Poisoned());

  // 2. Detect mode: literal vs enum
  bool has_literal = false, has_variant = false;
  for (auto &arm : ast->arms) {
    if (arm.pattern.is_wildcard) continue;
    if (arm.pattern.is_literal) has_literal = true;
    else has_variant = true;
  }

  if (has_literal && has_variant) {
    this->add_error(ast->get_location(),
                    "Cannot mix literal and enum variant patterns in case expression");
    return ast->set_type(Type::Poisoned());
  }

  // 3. Validate patterns based on mode
  bool had_error = false;

  if (has_literal) {
    // --- Literal mode ---
    // Resolve polymorphic scrutinee (e.g. Integer → i32)
    if (scrutinee_type.is_polymorphic_numeric()) {
      scrutinee_type = default_polymorphic_type(scrutinee_type);
      resolve_literal_type(ast->scrutinee.get(), scrutinee_type);
    }

    // Reject float scrutinees
    if (scrutinee_type.type_kind == TypeKind::F32_t ||
        scrutinee_type.type_kind == TypeKind::F64_t ||
        scrutinee_type.type_kind == TypeKind::Flt) {
      this->add_error(ast->scrutinee->get_location(),
                      fmt::format("Float types cannot be used in literal case "
                                  "patterns (float equality is unreliable), got {}",
                                  scrutinee_type.to_string()));
      return ast->set_type(Type::Poisoned());
    }

    // Must be integer, bool, or char
    if (!scrutinee_type.is_matchable_scalar()) {
      this->add_error(
          ast->scrutinee->get_location(),
          fmt::format("Literal case patterns require an integer, bool, or char "
                      "scrutinee type, got {}",
                      scrutinee_type.to_string()));
      return ast->set_type(Type::Poisoned());
    }

    // Validate each literal pattern
    using LK = CasePattern::LiteralKind;
    std::set<std::string> seen_literals;
    for (auto &arm : ast->arms) {
      if (arm.pattern.is_wildcard || !arm.pattern.is_literal) continue;

      // Check for duplicates
      if (!seen_literals.insert(arm.pattern.literal_value).second) {
        this->add_error(arm.pattern.location,
                        fmt::format("Duplicate literal pattern '{}'",
                                    arm.pattern.literal_value));
        had_error = true;
        continue;
      }

      // Validate literal kind matches scrutinee type
      if (scrutinee_type.type_kind == TypeKind::Bool) {
        if (arm.pattern.literal_kind != LK::Bool) {
          this->add_error(
              arm.pattern.location,
              fmt::format("Expected boolean pattern, got '{}'",
                          arm.pattern.literal_value));
          had_error = true;
        }
      } else if (scrutinee_type.type_kind == TypeKind::Char) {
        if (arm.pattern.literal_kind != LK::Char) {
          this->add_error(
              arm.pattern.location,
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
      bool has_wildcard = std::any_of(ast->arms.begin(), ast->arms.end(),
          [](const auto &a) { return a.pattern.is_wildcard; });

      if (scrutinee_type.type_kind == TypeKind::Bool) {
        // Bool: exhaustive if both true and false covered, or wildcard present
        if (!has_wildcard) {
          bool has_true = seen_literals.contains("true");
          bool has_false = seen_literals.contains("false");
          if (!has_true || !has_false) {
            std::string missing;
            if (!has_true) missing += "true";
            if (!has_false) {
              if (!missing.empty()) missing += ", ";
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
          this->add_error(
              ast->scrutinee->get_location(),
              fmt::format("Non-exhaustive case expression on {}: "
                          "a wildcard '_' pattern is required",
                          scrutinee_type.to_string()));
          had_error = true;
        }
      }
    }
  } else {
    // --- Enum mode (existing logic) ---
    if (scrutinee_type.type_kind != TypeKind::Enum) {
      this->add_error(
          ast->scrutinee->get_location(),
          fmt::format("Case expression requires an enum type, got {}",
                      scrutinee_type.to_string()));
      return ast->set_type(Type::Poisoned());
    }

    auto &et = std::get<EnumType>(scrutinee_type.type_data);

    for (auto &arm : ast->arms) {
      if (arm.pattern.is_wildcard) continue;

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
            if (!missing.empty()) missing += ", ";
            missing += et.get_variant(i).name;
          }
        }
        this->add_error(
            ast->scrutinee->get_location(),
            fmt::format(
                "Non-exhaustive case expression: missing variant(s) {}",
                missing));
        had_error = true;
      }
    }
  }

  // 4. Type-check arm bodies and unify result types
  // ASTContext is inherited on enter_new_scope(), so enclosing_function
  // is automatically available inside arm scopes — no manual capture needed.
  Type result_type = Type::Never();

  for (auto &arm : ast->arms) {
    if (had_error && !arm.pattern.is_wildcard && !arm.pattern.is_literal)
      continue; // skip arms with validation errors

    enter_new_scope();

    // Register enum payload bindings (only for non-wildcard, non-literal enum arms)
    if (!arm.pattern.is_wildcard && !arm.pattern.is_literal &&
        scrutinee_type.type_kind == TypeKind::Enum) {
      auto &et = std::get<EnumType>(scrutinee_type.type_data);
      auto &vi = et.get_variant(arm.pattern.variant_index);
      for (size_t i = 0; i < arm.pattern.bindings.size(); i++) {
        id_to_type.registerNameT(arm.pattern.bindings[i], vi.payload_types[i]);
      }
    }

    arm.body->accept_vis(this);
    auto arm_type = arm.body->accept_synthesis(this);
    exit_new_scope();

    // Unify with result type
    if (result_type.type_kind == TypeKind::Never) {
      result_type = arm_type;
    } else if (arm_type.type_kind != TypeKind::Never &&
               arm_type != result_type) {
      if (type_map_ordering.structurally_compatible(result_type, arm_type)) {
        // arm_type is compatible with result_type, keep result_type
      } else if (type_map_ordering.structurally_compatible(arm_type,
                                                           result_type)) {
        result_type = arm_type;
      } else {
        this->add_error(
            arm.body->get_location(),
            fmt::format(
                "Case arms have incompatible types: expected {}, got {}",
                result_type.to_string(), arm_type.to_string()));
        if (auto hint = incompatibility_hint(result_type, arm_type))
          this->add_diagnostics(arm.body->get_location(), *hint);
        had_error = true;
      }
    }
  }

  if (had_error)
    return ast->set_type(Type::Poisoned());

  return ast->set_type(result_type);
}

// --- Generic support: unification, substitution ---


} // namespace sammine_lang::AST
