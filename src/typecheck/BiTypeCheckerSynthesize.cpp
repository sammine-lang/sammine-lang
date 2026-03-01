#include "typecheck/BiTypeChecker.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "typecheck/Types.h"

#define DEBUG_TYPE "typecheck"
#include "util/Logging.h"

//! \file BiTypeCheckerSynthesize.cpp
//! \brief All synthesize() methods, call-dispatch helpers, and
//!        generic unification/substitution for BiTypeCheckerVisitor.
namespace sammine_lang::AST {

Type BiTypeCheckerVisitor::synthesize(ProgramAST *ast) {
  return Type::NonExistent();
}

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

  { auto t = ast->get_type(); t.is_mutable = ast->is_mutable; ast->set_type(t); }

  id_to_type.registerNameT(ast->TypedVar->name, ast->get_type());
  LOG({
    fmt::print(stderr, "[typecheck] synthesize VarDefAST: '{}' : {} ({}{})\n",
               ast->TypedVar->name, ast->get_type().to_string(),
               ast->is_mutable ? "mutable" : "immutable",
               ast->get_type().is_linear ? ", linear" : "");
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
        auto it = generic_enum_defs.find(base_name);
        if (it != generic_enum_defs.end()) {
          auto *generic_def = it->second;
          if (ast->explicit_type_args.size() == generic_def->type_params.size()) {
            Monomorphizer::SubstitutionMap bindings;
            std::string mangled = base_name + "<";
            bool ok = true;
            for (size_t i = 0; i < ast->explicit_type_args.size(); i++) {
              auto resolved = resolve_type_expr(ast->explicit_type_args[i].get());
              if (resolved.type_kind == TypeKind::Poisoned) { ok = false; break; }
              bindings[generic_def->type_params[i]] = resolved;
              if (i > 0) mangled += ", ";
              mangled += resolved.to_string();
            }
            mangled += ">";

            if (ok && !instantiated_enums.contains(mangled)) {
              auto cloned = Monomorphizer::instantiate_enum(generic_def, mangled, bindings);
              cloned->accept_vis(this);
              instantiated_enums.insert(mangled);
              monomorphized_enum_defs.push_back(std::move(cloned));
            }
            if (ok)
              enum_type_opt = get_typename_type(mangled);
          }
        }
      }
    }

    // Generic enum variant without explicit type args
    // (e.g., Some(42) rewritten to Option::Some(42) by scope generator)
    if (!enum_type_opt) {
      auto vc_it = variant_constructors.find(ast->functionName.get_name());
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
  if (generic_func_defs.contains(ast->functionName.mangled()))
    return synthesize_generic_call(ast);

  // Normal function call
  return synthesize_normal_call(ast);
}

std::optional<Type>
BiTypeCheckerVisitor::synthesize_typeclass_call(CallExprAST *ast) {
  auto method_name = ast->functionName.mangled();
  auto class_it = method_to_class.find(method_name);
  if (class_it == method_to_class.end())
    return std::nullopt; // Not a typeclass method — fall through

  auto &class_name = class_it->second;
  auto &tc = type_class_defs[class_name];

  Type concrete = resolve_type_expr(ast->explicit_type_args[0].get());
  if (concrete.type_kind == TypeKind::Poisoned)
    return ast->set_type(Type::Poisoned());

  std::string key = class_name + "__" + concrete.to_string();
  auto inst_it = type_class_instances.find(key);
  if (inst_it == type_class_instances.end()) {
    add_error(ast->get_location(),
              fmt::format("No instance of {}<{}>", class_name,
                          concrete.to_string()));
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

  props_.call(ast->id()).resolved_generic_name =
      inst_it->second.method_mangled_names[method_name];
  props_.call(ast->id()).is_typeclass_call = true;

  auto return_type = func_type.get_return_type();
  std::unordered_map<std::string, Type> bindings;
  bindings[tc.type_param] = concrete;
  return ast->set_type(substitute(return_type, bindings));
}

Type BiTypeCheckerVisitor::synthesize_generic_call(CallExprAST *ast) {
  auto *generic_def = generic_func_defs[ast->functionName.mangled()];
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

  std::unordered_map<std::string, Type> bindings;

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

    for (size_t i = 0; i < type_params.size(); i++) {
      Type resolved = resolve_type_expr(ast->explicit_type_args[i].get());
      if (resolved.type_kind == TypeKind::Poisoned)
        return ast->set_type(Type::Poisoned());
      bindings[type_params[i]] = resolved;
    }

    for (size_t i = 0; i < ast->arguments.size(); i++) {
      auto arg_type = ast->arguments[i]->accept_synthesis(this);
      if (arg_type.type_kind == TypeKind::Poisoned)
        return ast->set_type(Type::Poisoned());
      auto expected = substitute(params[i], bindings);
      if (!type_map_ordering.compatible_to_from(expected, arg_type)) {
        this->add_error(
            ast->arguments[i]->get_location(),
            fmt::format("Argument {} to '{}': expected {}, got {}", i + 1,
                        ast->functionName.mangled(), expected.to_string(),
                        arg_type.to_string()));
        if (auto hint = incompatibility_hint(expected, arg_type))
          this->add_diagnostics(ast->arguments[i]->get_location(), *hint);
        return ast->set_type(Type::Poisoned());
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
        auto expected = substitute(params[i], bindings);
        this->add_error(ast->arguments[i]->get_location(),
                        fmt::format("Type mismatch in argument {} of '{}': "
                                    "expected {}, got {}",
                                    i + 1, ast->functionName.mangled(),
                                    expected.to_string(),
                                    arg_type.to_string()));
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

  std::string mangled = ast->functionName.mangled() + "<";
  for (size_t i = 0; i < generic_def->Prototype->type_params.size(); i++) {
    if (i > 0) mangled += ", ";
    mangled += bindings[generic_def->Prototype->type_params[i]].to_string();
  }
  mangled += ">";

  props_.call(ast->id()).resolved_generic_name = mangled;
  props_.call(ast->id()).type_bindings = bindings;
  props_.call(ast->id()).callee_func_type = generic_type;
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

Type BiTypeCheckerVisitor::synthesize(ReturnExprAST *ast) {
  return ast->set_type(Type::Never());
}
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
    if (type_map_ordering.compatible_to_from(ast->RHS->get_type(), ast->LHS->get_type()))
      resolve_literal_type(ast->LHS.get(), ast->RHS->get_type());
  } else if (ast->RHS->get_type().is_polymorphic_numeric() &&
             !ast->LHS->get_type().is_polymorphic_numeric()) {
    if (type_map_ordering.compatible_to_from(ast->LHS->get_type(), ast->RHS->get_type()))
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
      if (!var->get_type().is_mutable) {
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
        if (!arr_var->get_type().is_mutable) {
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
          {TokADD, {"Add", "add"}}, {TokSUB, {"Sub", "sub"}},
          {TokMUL, {"Mul", "mul"}}, {TokDIV, {"Div", "div"}},
          {TokMOD, {"Mod", "mod"}},
      };

  auto it = op_to_class.find(ast->Op->tok_type);
  if (it != op_to_class.end()) {
    auto &[class_name, method_name] = it->second;
    std::string key = class_name + "__" + lhs_type.to_string();
    auto inst_it = type_class_instances.find(key);
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

  // Both branches must have compatible types
  if (then_type != else_type) {
    if (type_map_ordering.compatible_to_from(else_type, then_type)) {
      resolve_literal_type(ast->thenBlockAST->Statements.back().get(),
                           else_type);
      ast->thenBlockAST->set_type(else_type);
      return ast->set_type(else_type);
    } else if (type_map_ordering.compatible_to_from(then_type, else_type)) {
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

  ast_type.is_mutable = ast->is_mutable;
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

  { auto t = Type::Pointer(element_type); t.is_linear = true; ast->set_type(t); } // alloc always produces linear pointers
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
Type BiTypeCheckerVisitor::synthesize(IndexExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  auto arr_type = ast->array_expr->accept_synthesis(this);
  if (arr_type.type_kind != TypeKind::Array) {
    this->add_error(
        ast->get_location(),
        fmt::format("Cannot index non-array type '{}'", arr_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

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
                    fmt::format("Array index must be integer, got '{}'",
                                idx_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  auto &arr_data = std::get<ArrayType>(arr_type.type_data);
  if (auto *num = llvm::dyn_cast<NumberExprAST>(ast->index_expr.get())) {
    int idx = std::stoi(num->number);
    int size = static_cast<int>(arr_data.get_size());
    if (idx < 0 || idx >= size) {
      this->add_error(
          ast->get_location(),
          fmt::format("Array index out of bounds: index {} on array of size {}",
                      idx, size));
      return ast->set_type(Type::Poisoned());
    }
  }

  return ast->set_type(arr_data.get_element());
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
        fmt::format("Struct '{}' has no field named '{}'", st.get_name(),
                    ast->field_name));
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

Type BiTypeCheckerVisitor::synthesize(CaseExprAST *ast) {
  if (ast->synthesized())
    return ast->get_type();

  // 1. Synthesize scrutinee — must be an enum type
  auto scrutinee_type = ast->scrutinee->accept_synthesis(this);
  if (scrutinee_type.type_kind == TypeKind::Poisoned)
    return ast->set_type(Type::Poisoned());

  if (scrutinee_type.type_kind != TypeKind::Enum) {
    this->add_error(ast->scrutinee->get_location(),
                    fmt::format("Case expression requires an enum type, got {}",
                                scrutinee_type.to_string()));
    return ast->set_type(Type::Poisoned());
  }

  auto &et = std::get<EnumType>(scrutinee_type.type_data);

  // 2. Process each arm: validate pattern, type-check body with bindings in scope
  // Capture the enclosing function scope so return statements work inside arms
  auto enclosing_scope = id_to_type.top().s;

  Type result_type = Type::Never();
  bool had_error = false;

  for (auto &arm : ast->arms) {
    if (arm.pattern.is_wildcard) {
      // Wildcard: no bindings, just type-check the body
      enter_new_scope();
      if (enclosing_scope.has_value())
        id_to_type.top().setScope(enclosing_scope.value());
      arm.body->accept_vis(this);
      auto arm_type = arm.body->accept_synthesis(this);
      exit_new_scope();

      // Unify with result type
      if (result_type.type_kind == TypeKind::Never) {
        result_type = arm_type;
      } else if (arm_type.type_kind != TypeKind::Never &&
                 arm_type != result_type) {
        if (type_map_ordering.compatible_to_from(result_type, arm_type)) {
          // arm_type is compatible with result_type, keep result_type
        } else if (type_map_ordering.compatible_to_from(arm_type, result_type)) {
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
      continue;
    }

    // Non-wildcard: look up variant in enum
    auto variant_name = arm.pattern.variant_name.get_name();
    auto variant_idx = et.get_variant_index(variant_name);
    if (!variant_idx.has_value()) {
      this->add_error(arm.pattern.location,
                      fmt::format("Type '{}' has no variant '{}'",
                                  et.get_name().mangled(), variant_name));
      had_error = true;
      continue;
    }

    // Store the resolved variant index for codegen
    arm.pattern.variant_index = variant_idx.value();
    auto &vi = et.get_variant(variant_idx.value());

    // Validate binding count
    if (arm.pattern.bindings.size() != vi.payload_types.size()) {
      this->add_error(
          arm.pattern.location,
          fmt::format("Pattern '{}::{}' expects {} bindings, got {}",
                      et.get_name().mangled(), variant_name,
                      vi.payload_types.size(), arm.pattern.bindings.size()));
      had_error = true;
      continue;
    }

    // Enter a new scope and register bindings with their payload types
    enter_new_scope();
    if (enclosing_scope.has_value())
      id_to_type.top().setScope(enclosing_scope.value());
    for (size_t i = 0; i < arm.pattern.bindings.size(); i++) {
      id_to_type.registerNameT(arm.pattern.bindings[i], vi.payload_types[i]);
    }

    // Type-check the arm body
    arm.body->accept_vis(this);
    auto arm_type = arm.body->accept_synthesis(this);
    exit_new_scope();

    // Unify with result type (same Never logic as IfExprAST)
    if (result_type.type_kind == TypeKind::Never) {
      result_type = arm_type;
    } else if (arm_type.type_kind != TypeKind::Never &&
               arm_type != result_type) {
      if (type_map_ordering.compatible_to_from(result_type, arm_type)) {
        // arm_type is compatible with result_type, keep result_type
      } else if (type_map_ordering.compatible_to_from(arm_type, result_type)) {
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

  // 3. Exhaustiveness check: ensure all variants are covered
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
          fmt::format("Non-exhaustive case expression: missing variant(s) {}",
                      missing));
      had_error = true;
    }
  }

  if (had_error)
    return ast->set_type(Type::Poisoned());

  return ast->set_type(result_type);
}

// --- Generic support: unification, substitution ---

bool BiTypeCheckerVisitor::contains_type_param(const Type &type,
                                               const std::string &param_name) {
  if (type.type_kind == TypeKind::TypeParam)
    return std::get<std::string>(type.type_data) == param_name;

  bool found = false;
  type.forEachInnerType([&](const Type &inner) {
    if (!found && contains_type_param(inner, param_name))
      found = true;
  });
  return found;
}

bool BiTypeCheckerVisitor::unify(
    const Type &pattern, const Type &concrete,
    std::unordered_map<std::string, Type> &bindings) {
  if (pattern.type_kind == TypeKind::TypeParam) {
    auto name = std::get<std::string>(pattern.type_data);
    // Occurs check
    if (contains_type_param(concrete, name))
      return false;
    auto it = bindings.find(name);
    if (it != bindings.end()) {
      return it->second == concrete;
    }
    bindings[name] = concrete;
    return true;
  }

  if (pattern.type_kind != concrete.type_kind)
    return false;

  if (pattern.type_kind == TypeKind::Pointer) {
    auto pp = std::get<PointerType>(pattern.type_data).get_pointee();
    auto cp = std::get<PointerType>(concrete.type_data).get_pointee();
    return unify(pp, cp, bindings);
  }
  if (pattern.type_kind == TypeKind::Array) {
    auto pa = std::get<ArrayType>(pattern.type_data);
    auto ca = std::get<ArrayType>(concrete.type_data);
    if (pa.get_size() != ca.get_size())
      return false;
    return unify(pa.get_element(), ca.get_element(), bindings);
  }
  if (pattern.type_kind == TypeKind::Function) {
    auto pf = std::get<FunctionType>(pattern.type_data);
    auto cf = std::get<FunctionType>(concrete.type_data);
    auto pp = pf.get_params_types();
    auto cp = cf.get_params_types();
    if (pp.size() != cp.size())
      return false;
    for (size_t i = 0; i < pp.size(); i++)
      if (!unify(pp[i], cp[i], bindings))
        return false;
    return unify(pf.get_return_type(), cf.get_return_type(), bindings);
  }
  if (pattern.type_kind == TypeKind::Tuple) {
    auto &pt = std::get<TupleType>(pattern.type_data);
    auto &ct = std::get<TupleType>(concrete.type_data);
    if (pt.size() != ct.size())
      return false;
    for (size_t i = 0; i < pt.size(); i++)
      if (!unify(pt.get_element(i), ct.get_element(i), bindings))
        return false;
    return true;
  }

  return true;
}

Type BiTypeCheckerVisitor::substitute(
    const Type &type, const std::unordered_map<std::string, Type> &bindings) {
  if (type.type_kind == TypeKind::TypeParam) {
    auto name = std::get<std::string>(type.type_data);
    auto it = bindings.find(name);
    if (it != bindings.end())
      return it->second;
    return type;
  }
  if (type.type_kind == TypeKind::Pointer) {
    auto pointee = std::get<PointerType>(type.type_data).get_pointee();
    return Type::Pointer(substitute(pointee, bindings));
  }
  if (type.type_kind == TypeKind::Array) {
    auto arr = std::get<ArrayType>(type.type_data);
    return Type::Array(substitute(arr.get_element(), bindings), arr.get_size());
  }
  if (type.type_kind == TypeKind::Function) {
    auto fn = std::get<FunctionType>(type.type_data);
    std::vector<Type> total;
    for (auto &p : fn.get_params_types())
      total.push_back(substitute(p, bindings));
    total.push_back(substitute(fn.get_return_type(), bindings));
    return Type::Function(std::move(total));
  }
  if (type.type_kind == TypeKind::Tuple) {
    auto &tt = std::get<TupleType>(type.type_data);
    std::vector<Type> elems;
    for (auto &e : tt.get_element_types())
      elems.push_back(substitute(e, bindings));
    return Type::Tuple(std::move(elems));
  }
  return type;
}

} // namespace sammine_lang::AST
