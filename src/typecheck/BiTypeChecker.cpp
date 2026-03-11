#include "typecheck/BiTypeChecker.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "util/MonomorphizedName.h"
#include "util/Utilities.h"

#define DEBUG_TYPE "typecheck"
#include "util/Logging.h"

//! \file BiTypeChecker.cpp
//! \brief Bidirectional type checker. Uses two operations:
//!   - synthesize(): infer a type bottom-up from an expression (in BiTypeCheckerSynthesize.cpp)
//!   - check/visit(): validate against an expected type top-down
//! Three-pass approach over ProgramAST:
//!   1) Register all type definitions (structs, enums, typeclasses, aliases)
//!   2) Pre-register all function signatures (enables mutual recursion)
//!   3) Full type checking of function bodies + expressions
namespace sammine_lang::AST {

// visit overrides — explicit traversal order

void BiTypeCheckerVisitor::pre_register_function(PrototypeAST *ast) {
  // Build the function type by resolving type expressions directly,
  // without calling accept_synthesis (which would mark TypedVarASTs
  // as synthesized and prevent parameter re-registration in the full pass).
  std::vector<Type> types;
  for (auto &param : ast->parameterVectors)
    types.push_back(resolve_type_expr(param->type_expr.get()));
  if (ast->returnsUnit())
    types.push_back(Type::Unit());
  else
    types.push_back(resolve_type_expr(ast->return_type_expr.get()));
  auto fn_type = Type::Function(std::move(types), ast->is_var_arg);
  id_to_type.registerNameT(ast->functionName.mangled(), fn_type);
}

void BiTypeCheckerVisitor::visit(ProgramAST *ast) {
  top_level_ast = ast;

  // First pass: register structs, enums, type aliases, typeclass declarations,
  // and instances so method bodies can reference any type regardless of order.
  for (auto &def : ast->DefinitionVec) {
    if (auto *sd = llvm::dyn_cast<StructDefAST>(def.get()))
      sd->accept_vis(this);
    else if (auto *ed = llvm::dyn_cast<EnumDefAST>(def.get()))
      ed->accept_vis(this);
    else if (auto *ta = llvm::dyn_cast<TypeAliasDefAST>(def.get()))
      ta->accept_vis(this);
    else if (auto *tc = llvm::dyn_cast<TypeClassDeclAST>(def.get()))
      register_typeclass_decl(tc);
    else if (auto *tci = llvm::dyn_cast<TypeClassInstanceAST>(def.get()))
      register_typeclass_instance(tci);
  }

  // Register compiler-builtin operator instances for primitive types
  // (Add/Sub/Mul/Div/Mod on i32, i64, f64, char) — no source bodies,
  // codegen emits inline ops for these.
  register_builtin_op_instances();

  // Second pass: pre-register all function signatures so mutual recursion works
  for (auto &def : ast->DefinitionVec) {
    if (auto *fd = llvm::dyn_cast<FuncDefAST>(def.get())) {
      if (fd->Prototype->is_generic())
        monomorphizer.register_generic_func(fd->Prototype->functionName.mangled(), fd);
      else
        pre_register_function(fd->Prototype.get());
    } else if (auto *ext = llvm::dyn_cast<ExternAST>(def.get())) {
      if (!ext->Prototype->is_generic())
        pre_register_function(ext->Prototype.get());
    } else if (auto *tci = llvm::dyn_cast<TypeClassInstanceAST>(def.get())) {
      for (auto &method : tci->methods)
        if (!method->Prototype->is_generic())
          pre_register_function(method->Prototype.get());
    } else if (auto *kd = llvm::dyn_cast<KernelDefAST>(def.get())) {
      if (!kd->Prototype->is_generic())
        pre_register_function(kd->Prototype.get());
    }
  }

  // Third pass: full type checking of everything
  for (auto &def : ast->DefinitionVec)
    def->accept_vis(this);
}

void BiTypeCheckerVisitor::visit(FuncDefAST *ast) {
  enter_new_scope();
  ctx().enclosing_function = ast;

  if (ast->Prototype->is_var_arg) {
    this->add_error(
        ast->Prototype->get_location(),
        "Variadic arguments ('...') are only supported on extern declarations "
        "for C interop, not on function definitions");
  }

  // Register explicitly-declared type parameters before visiting the prototype
  for (auto &tp_name : ast->Prototype->type_params) {
    auto tp = Type::TypeParam(tp_name);
    typename_to_type.registerNameT(tp_name, tp);
  }

  // Visit prototype (resolves param/return types, registers function name)
  // Previously done inside discover_type_params().
  ast->Prototype->accept_vis(this);
  ast->accept_synthesis(this);

  if (ast->Prototype->is_generic()) {
    monomorphizer.register_generic_func(ast->Prototype->functionName.mangled(), ast);
  } else {
    ast->Block->accept_vis(this);
  }

  exit_new_scope();
}

void BiTypeCheckerVisitor::visit(PrototypeAST *ast) {
  ast->accept_synthesis(this);
  for (auto &var : ast->parameterVectors)
    var->accept_vis(this);
  id_to_type.parent_scope()->registerNameT(ast->functionName.mangled(),
                                           ast->get_type());
  if (ast->functionName.get_name() == "main") {
    auto fn_type = std::get<FunctionType>(ast->get_type().type_data);
    auto return_type = fn_type.get_return_type();
    if (return_type != Type::I32_t()) {
      this->add_error(ast->get_location(),
                      fmt::format("main must return i32, found {}",
                                  return_type.to_string()));
    }
    // Validate parameters: either 0 args or (i32, ptr<ptr<char>>)
    auto param_count = ast->parameterVectors.size();
    if (param_count != 0 && param_count != 2) {
      this->add_error(
          ast->get_location(),
          "main must take 0 or 2 parameters (argc: i32, argv: ptr<ptr<char>>)");
    }
    if (param_count == 2) {
      auto argc_type = ast->parameterVectors[0]->get_type();
      auto argv_type = ast->parameterVectors[1]->get_type();
      if (argc_type != Type::I32_t()) {
        this->add_error(ast->parameterVectors[0]->get_location(),
                        fmt::format("main's first parameter must be i32 "
                                    "(argc), found {}",
                                    argc_type.to_string()));
      }
      if (argv_type != Type::Pointer(Type::Pointer(Type::Char()))) {
        this->add_error(ast->parameterVectors[1]->get_location(),
                        fmt::format("main's second parameter must be "
                                    "ptr<ptr<char>> (argv), found {}",
                                    argv_type.to_string()));
      }
    }
  }
}

bool BiTypeCheckerVisitor::check_array_literal_against_annotation(
    ArrayLiteralExprAST *arr_lit, const ArrayType &arr_type) {
  auto expected_elem = arr_type.get_element();
  size_t first_error_idx = 0;
  size_t error_count = 0;
  for (size_t i = 0; i < arr_lit->elements.size(); i++) {
    auto elem_type = arr_lit->elements[i]->accept_synthesis(this);
    if (!type_map_ordering.compatible_to_from(expected_elem, elem_type)) {
      if (error_count == 0)
        first_error_idx = i;
      error_count++;
    } else {
      resolve_literal_type(arr_lit->elements[i].get(), expected_elem);
    }
  }
  if (error_count > 0) {
    std::vector<std::string> msgs;
    msgs.push_back(
        fmt::format("Array element type mismatch: expected {}, got {}",
                    expected_elem.to_string(),
                    arr_lit->elements[first_error_idx]
                        ->accept_synthesis(this)
                        .to_string()));
    if (error_count > 1)
      msgs.push_back(fmt::format("{} more type mismatch {} in this array",
                                 error_count - 1,
                                 (error_count - 1 == 1) ? "error" : "errors"));
    this->add_error(arr_lit->elements[first_error_idx]->get_location(), msgs);
    if (auto hint = incompatibility_hint(
            expected_elem,
            arr_lit->elements[first_error_idx]->accept_synthesis(this)))
      this->add_diagnostics(
          arr_lit->elements[first_error_idx]->get_location(), *hint);
    arr_lit->set_type(Type::Poisoned());
    return true;
  }
  return false;
}

void BiTypeCheckerVisitor::visit(VarDefAST *ast) {
  // Tuple destructuring: let (a, b) = expr;
  if (ast->is_tuple_destructure) {
    ast->Expression->accept_vis(this);
    ast->accept_synthesis(this);
    return;
  }

  // Pre-resolve type annotation to trigger generic enum instantiation
  // (ensures variant_constructors is populated for unqualified constructors)
  if (ast->TypedVar->type_expr != nullptr)
    resolve_type_expr(ast->TypedVar->type_expr.get());

  // Special case: array type annotation + array literal RHS
  if (ast->TypedVar->type_expr != nullptr) {
    if (auto *arr_lit =
            llvm::dyn_cast<ArrayLiteralExprAST>(ast->Expression.get())) {
      ast->accept_synthesis(this);
      if (ast->get_type().type_kind == TypeKind::Array) {
        auto arr_data = std::get<ArrayType>(ast->get_type().type_data);
        for (auto &elem : arr_lit->elements)
          elem->accept_vis(this);
        if (check_array_literal_against_annotation(arr_lit, arr_data)) {
          ast->set_type(Type::Poisoned());
          return;
        }
        arr_lit->set_type(ast->get_type());
        return;
      }
    }
  }

  // Normal case
  ast->Expression->accept_vis(this);
  ast->accept_synthesis(this);
  auto to = ast->get_type();
  auto from = ast->Expression->accept_synthesis(this);
  if (to == Type::Poisoned() || from == Type::Poisoned()) {
    ast->set_type(Type::Poisoned());
  } else if (!type_map_ordering.compatible_to_from(to, from)) {
    this->add_error(ast->Expression->get_location(),
                    fmt::format("Type mismatch: expression has type {}, "
                                "but variable declared as {}",
                                from.to_string(), to.to_string()));
    if (auto hint = incompatibility_hint(to, from))
      this->add_diagnostics(ast->Expression->get_location(), *hint);
    ast->set_type(Type::Poisoned());
  } else if (from.is_polymorphic_numeric()) {
    resolve_literal_type(ast->Expression.get(), to);
  }
}

void BiTypeCheckerVisitor::visit(ExternAST *ast) {
  enter_new_scope();
  if (ast->Prototype->is_generic()) {
    this->add_error(
        ast->Prototype->get_location(),
        "Generic reuse declarations are not supported; generics require a "
        "function body for monomorphization");
    exit_new_scope();
    return;
  }
  ast->Prototype->accept_vis(this);
  exit_new_scope();
}

void BiTypeCheckerVisitor::visit(StructDefAST *ast) {
  // Generic struct: register as template, skip concrete registration
  if (!ast->type_params.empty()) {
    if (!ast->get_type().synthesized()) {
      monomorphizer.register_generic_struct(ast->struct_name.mangled(), ast);
      ast->set_type(Type::Generic()); // mark as visited (not concretely usable)
    }
    return;
  }

  // Skip if already registered (e.g. by first pass)
  if (ast->get_type().synthesized())
    return;

  std::vector<std::string> field_names;
  std::vector<Type> field_types;

  for (auto &member : ast->struct_members) {
    auto ft = resolve_type_expr(member->type_expr.get());
    field_names.push_back(member->name);
    field_types.push_back(ft);
    if (ft.type_kind == TypeKind::Poisoned) {
      ast->set_type(Type::Poisoned());
      return;
    }
  }

  auto struct_type =
      Type::Struct(ast->struct_name, std::move(field_names),
                   std::move(field_types));
  ast->set_type(struct_type);
  typename_to_type.registerNameTAtRoot(ast->struct_name.mangled(), struct_type);
}

void BiTypeCheckerVisitor::visit(EnumDefAST *ast) {
  // Generic enum: register as template, skip concrete registration
  if (!ast->type_params.empty()) {
    if (!ast->get_type().synthesized()) {
      monomorphizer.register_generic_enum(ast->enum_name.mangled(), ast);
      ast->set_type(Type::Generic()); // mark as visited (not concretely usable)
    }
    return;
  }

  // Resolve backing type if specified
  TypeKind backing_type = TypeKind::I32_t;
  if (ast->backing_type_name.has_value()) {
    static const std::unordered_map<std::string, TypeKind> backing_type_map = {
        {"i32", TypeKind::I32_t},
        {"i64", TypeKind::I64_t},
        {"u32", TypeKind::U32_t},
        {"u64", TypeKind::U64_t},
    };
    auto &bt_name = ast->backing_type_name.value();
    auto it = backing_type_map.find(bt_name);
    if (it != backing_type_map.end()) {
      backing_type = it->second;
    } else {
      this->add_error(
          ast->get_location(),
          fmt::format("Invalid backing type '{}' for type '{}' — must be "
                      "i32, i64, u32, or u64",
                      bt_name, ast->enum_name.mangled()));
      ast->set_type(Type::Poisoned());
      return;
    }
    // Backing type annotation implies integer-backed
    ast->is_integer_backed = true;
  }

  // Validate integer-backed enums: all variants must have discriminant values,
  // no mixing with type payloads
  if (ast->is_integer_backed) {
    std::set<int64_t> seen_values;
    for (auto &variant : ast->variants) {
      if (!variant.discriminant_value.has_value()) {
        this->add_error(
            variant.location,
            fmt::format("Integer-backed enum '{}': variant '{}' is missing a "
                        "discriminant value — all variants must have one",
                        ast->enum_name.mangled(), variant.name));
        ast->set_type(Type::Poisoned());
        return;
      }
      if (!variant.payload_types.empty()) {
        this->add_error(
            variant.location,
            fmt::format("Integer-backed enum '{}': variant '{}' cannot have "
                        "both a discriminant value and type payloads",
                        ast->enum_name.mangled(), variant.name));
        ast->set_type(Type::Poisoned());
        return;
      }
      auto val = variant.discriminant_value.value();
      if (seen_values.contains(val)) {
        this->add_error(
            variant.location,
            fmt::format("Integer-backed enum '{}': duplicate discriminant "
                        "value {} on variant '{}'",
                        ast->enum_name.mangled(), val, variant.name));
        ast->set_type(Type::Poisoned());
        return;
      }
      seen_values.insert(val);
    }
  }

  std::vector<EnumType::VariantInfo> variant_infos;

  for (auto &variant : ast->variants) {
    EnumType::VariantInfo info;
    info.name = variant.name;
    info.discriminant_value = variant.discriminant_value;
    for (auto &type_expr : variant.payload_types) {
      auto resolved = resolve_type_expr(type_expr.get());
      if (resolved.type_kind == TypeKind::Poisoned) {
        ast->set_type(Type::Poisoned());
        return;
      }
      info.payload_types.push_back(resolved);
    }
    variant_infos.push_back(std::move(info));
  }

  auto enum_type = Type::Enum(ast->enum_name, std::move(variant_infos),
                              ast->is_integer_backed, backing_type);
  ast->set_type(enum_type);
  typename_to_type.registerNameTAtRoot(ast->enum_name.mangled(), enum_type);

  // Add lattice edge: integer-backed enum → backing type
  if (ast->is_integer_backed) {
    auto backing = [&]() -> Type {
      switch (backing_type) {
      case TypeKind::I32_t: return Type::I32_t();
      case TypeKind::I64_t: return Type::I64_t();
      case TypeKind::U32_t: return Type::U32_t();
      case TypeKind::U64_t: return Type::U64_t();
      default: return Type::I32_t();
      }
    }();
    type_map_ordering.type_map[enum_type] = backing;
  }

  // Register variant constructors with qualified keys (enum_name::variant_name)
  // so lookups match the fully-qualified names produced by scope generator
  auto &et = std::get<EnumType>(enum_type.type_data);
  for (size_t i = 0; i < et.variant_count(); i++) {
    auto &vi = et.get_variant(i);
    auto qualified_key = ast->enum_name.mangled() + "::" + vi.name;
    variant_constructors[qualified_key] = {enum_type, i};

    // For generic enum instantiations (e.g., Option<i32>), also register with
    // the template base name (Option::Some) so scope-gen-rewritten names match
    auto base_parts = ast->enum_name.parts();
    auto &last_part = base_parts.back();
    auto angle_pos = last_part.find('<');
    if (angle_pos != std::string::npos) {
      base_parts.back() = last_part.substr(0, angle_pos);
      auto base_key = sammine_util::QualifiedName::from_parts(base_parts).mangled()
                      + "::" + vi.name;
      variant_constructors[base_key] = {enum_type, i};
    }
  }
}

void BiTypeCheckerVisitor::visit(TypeAliasDefAST *ast) {
  // Already resolved — skip on third pass
  auto &tap = props_.type_alias(ast->id());
  if (tap.resolved_type.synthesized())
    return;

  auto resolved = resolve_type_expr(ast->type_expr.get());
  if (resolved.type_kind == TypeKind::Poisoned) {
    this->add_error(
        ast->get_location(),
        fmt::format("Cannot resolve type '{}' in type alias '{}'",
                    ast->type_expr->to_string(), ast->alias_name.mangled()));
    ast->set_type(Type::Poisoned());
    return;
  }
  tap.resolved_type = resolved;
  ast->set_type(resolved);
  typename_to_type.registerNameT(ast->alias_name.mangled(), resolved);
}

void BiTypeCheckerVisitor::visit(CallExprAST *ast) {
  ast->accept_synthesis(this);
  for (auto &arg : ast->arguments)
    arg->accept_vis(this);

  auto &cp = props_.call(ast->id());

  // Enum constructors are fully resolved during synthesis
  if (cp.is_enum_constructor)
    return;

  // Generic calls: trigger monomorphization if synthesis succeeded
  if (auto *generic_def =
          monomorphizer.find_generic_func(ast->functionName.mangled())) {
    if (cp.resolved_name.has_value()) {
      if (auto *cloned = monomorphizer.try_instantiate_func(
              generic_def, *cp.resolved_name, cp.type_bindings)) {
        cloned->accept_vis(this);
      }
    }
    return;
  }

  // Typeclass calls: nothing to do here — codegen reads resolved_name
  if (cp.is_typeclass_call)
    return;

  // Normal calls: check arg types now that args have been visited
  if (!cp.callee_func_type.has_value() ||
      cp.callee_func_type->type_kind != TypeKind::Function)
    return;

  auto func = std::get<FunctionType>(cp.callee_func_type->type_data);
  if (func.is_var_arg())
    return;

  auto params = func.get_params_types();
  for (size_t i = 0; i < ast->arguments.size(); i++) {
    if (!this->type_map_ordering.compatible_to_from(params[i],
                                                    ast->arguments[i]->get_type())) {
      this->add_error(ast->arguments[i]->get_location(),
                      fmt::format("Argument {} to '{}': expected {}, got {}",
                                  i + 1, ast->functionName.with_alias().mangled(),
                                  params[i].to_string(),
                                  ast->arguments[i]->get_type().to_string()));
      if (auto hint = incompatibility_hint(params[i], ast->arguments[i]->get_type()))
        this->add_diagnostics(ast->arguments[i]->get_location(), *hint);
      this->add_diagnostics(
          ast->arguments[i]->get_location(),
          fmt::format("note: '{}' has signature: {}",
                      ast->functionName.with_alias().mangled(),
                      cp.callee_func_type->to_string()));
    } else if (ast->arguments[i]->get_type().is_polymorphic_numeric()) {
      resolve_literal_type(ast->arguments[i].get(), params[i]);
    }
  }
}

void BiTypeCheckerVisitor::visit(ReturnExprAST *ast) {
  // Special case: array literal return — propagate function return type
  // into array elements before they default to i32.
  if (auto *arr_lit =
          llvm::dyn_cast<ArrayLiteralExprAST>(ast->return_expr.get())) {
    auto scope_fn = this->ctx().enclosing_function;
    auto fn_type = std::get<FunctionType>(scope_fn->get_type().type_data);
    auto return_type = fn_type.get_return_type();
    if (return_type.type_kind == TypeKind::Array) {
      auto &arr_data = std::get<ArrayType>(return_type.type_data);
      for (auto &elem : arr_lit->elements)
        elem->accept_vis(this);
      check_array_literal_against_annotation(arr_lit, arr_data);
      arr_lit->set_type(return_type);
      ast->accept_synthesis(this);
      return;
    }
  }

  // Normal case
  if (ast->return_expr)
    ast->return_expr->accept_vis(this);
  ast->accept_synthesis(this);
  auto t = ast->return_expr->accept_synthesis(this);
  auto scope_fn = this->ctx().enclosing_function;
  auto fn_type = std::get<FunctionType>(scope_fn->get_type().type_data);
  auto return_type = fn_type.get_return_type();
  if (!type_map_ordering.compatible_to_from(return_type, t)) {
    this->add_error(ast->get_location(),
                    fmt::format("Wrong return type for function {}, expected "
                                "{} but got {}",
                                scope_fn->getFunctionName(),
                                return_type.to_string(), t.to_string()));
    if (auto hint = incompatibility_hint(return_type, t))
      this->add_diagnostics(ast->get_location(), *hint);
  } else if (t.is_polymorphic_numeric()) {
    resolve_literal_type(ast->return_expr.get(), return_type);
  }
}

void BiTypeCheckerVisitor::visit(BinaryExprAST *ast) {
  ast->LHS->accept_vis(this);
  ast->RHS->accept_vis(this);
  ast->set_type(ast->accept_synthesis(this));
}

void BiTypeCheckerVisitor::visit(BlockAST *ast) {
  for (auto &stmt : ast->Statements)
    stmt->accept_vis(this);
}

void BiTypeCheckerVisitor::visit(IfExprAST *ast) {
  ast->bool_expr->accept_vis(this);
  ast->thenBlockAST->accept_vis(this);
  ast->elseBlockAST->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(NumberExprAST *ast) {
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(StringExprAST *ast) {
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(BoolExprAST *ast) {
  ast->accept_synthesis(this);
}
void BiTypeCheckerVisitor::visit(CharExprAST *ast) {
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(UnitExprAST *ast) {
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(VariableExprAST *ast) {
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(TypedVarAST *ast) {
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(DerefExprAST *ast) {
  ast->operand->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(AddrOfExprAST *ast) {
  ast->operand->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(AllocExprAST *ast) {
  ast->operand->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(FreeExprAST *ast) {
  ast->operand->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(ArrayLiteralExprAST *ast) {
  for (auto &elem : ast->elements)
    elem->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(RangeExprAST *ast) {
  ast->start->accept_vis(this);
  ast->end->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(IndexExprAST *ast) {
  ast->array_expr->accept_vis(this);
  ast->index_expr->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(LenExprAST *ast) {
  ast->operand->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(DimExprAST *ast) {
  ast->operand->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(UnaryNegExprAST *ast) {
  ast->operand->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(StructLiteralExprAST *ast) {
  for (auto &val : ast->field_values)
    val->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(FieldAccessExprAST *ast) {
  ast->object_expr->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(CaseExprAST *ast) {
  // Scrutinee visiting and arm visiting are done inside synthesize()
  // because arms need per-arm scoping with payload bindings
  ast->scrutinee->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(WhileExprAST *ast) {
  if (ast->condition)
    ast->condition->accept_vis(this);
  if (ast->body)
    ast->body->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(TupleLiteralExprAST *ast) {
  for (auto &elem : ast->elements)
    if (elem)
      elem->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::register_typeclass_decl(TypeClassDeclAST *ast) {
  if (type_class_defs.contains(ast->class_name))
    return;

  TypeClassInfo info;
  info.name = ast->class_name;
  info.type_params = ast->type_params;

  enter_new_scope();
  for (auto &param : ast->type_params)
    typename_to_type.registerNameT(param, Type::TypeParam(param));

  for (auto &proto : ast->methods) {
    proto->accept_synthesis(this);
    info.methods.push_back(proto.get());
    method_to_class[proto->functionName.mangled()] = ast->class_name;
  }

  exit_new_scope();
  type_class_defs[ast->class_name] = std::move(info);
}

void BiTypeCheckerVisitor::register_typeclass_instance(
    TypeClassInstanceAST *ast) {
  auto &tcip = props_.type_class_instance(ast->id());

  for (auto &type_expr : ast->concrete_type_exprs) {
    auto resolved = resolve_type_expr(type_expr.get());
    if (resolved.is_poisoned())
      return;
    tcip.concrete_types.push_back(resolved);
  }

  auto it = type_class_defs.find(ast->class_name);
  if (it == type_class_defs.end()) {
    this->add_error(ast->get_location(),
                    fmt::format("Unknown type class '{}'", ast->class_name));
    return;
  }

  auto &tc = it->second;
  if (tcip.concrete_types.size() != tc.type_params.size()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Type class '{}' expects {} type argument(s), got {}",
                    ast->class_name, tc.type_params.size(),
                    tcip.concrete_types.size()));
    return;
  }

  // Build comma-joined type string for instance key
  std::string concrete_types_str;
  for (size_t i = 0; i < tcip.concrete_types.size(); i++) {
    if (i > 0)
      concrete_types_str += ", ";
    concrete_types_str += tcip.concrete_types[i].to_string();
  }

  TypeClassInstanceInfo inst_info;
  inst_info.class_name = ast->class_name;
  inst_info.concrete_types = tcip.concrete_types;

  for (auto &method : ast->methods) {
    std::string original_name = method->Prototype->functionName.get_name();
    auto mono = sammine_util::MonomorphizedName::typeclass(
        ast->class_name, concrete_types_str, original_name);
    method->Prototype->functionName = mono.to_qualified_name();
    inst_info.method_mangled_names[original_name] = mono;
  }

  auto inst_key = sammine_util::MonomorphizedName::typeclass(
      ast->class_name, concrete_types_str, "");
  type_class_instances[inst_key.instance_key()] = std::move(inst_info);
}

void BiTypeCheckerVisitor::register_builtin_op_instances() {
  struct BuiltinEntry {
    const char *class_name;
    const char *method_name;
    Type type;
  };

  static const BuiltinEntry entries[] = {
      {"Add", "Add", Type::I32_t()}, {"Add", "Add", Type::I64_t()},
      {"Add", "Add", Type::U32_t()}, {"Add", "Add", Type::U64_t()},
      {"Add", "Add", Type::F64_t()}, {"Add", "Add", Type::F32_t()},
      {"Add", "Add", Type::Char()},
      {"Sub", "Sub", Type::I32_t()}, {"Sub", "Sub", Type::I64_t()},
      {"Sub", "Sub", Type::U32_t()}, {"Sub", "Sub", Type::U64_t()},
      {"Sub", "Sub", Type::F64_t()}, {"Sub", "Sub", Type::F32_t()},
      {"Mul", "Mul", Type::I32_t()},
      {"Mul", "Mul", Type::I64_t()}, {"Mul", "Mul", Type::U32_t()},
      {"Mul", "Mul", Type::U64_t()}, {"Mul", "Mul", Type::F64_t()},
      {"Mul", "Mul", Type::F32_t()},
      {"Div", "Div", Type::I32_t()}, {"Div", "Div", Type::I64_t()},
      {"Div", "Div", Type::U32_t()}, {"Div", "Div", Type::U64_t()},
      {"Div", "Div", Type::F64_t()}, {"Div", "Div", Type::F32_t()},
      {"Mod", "Mod", Type::I32_t()},
      {"Mod", "Mod", Type::I64_t()}, {"Mod", "Mod", Type::U32_t()},
      {"Mod", "Mod", Type::U64_t()},
  };

  for (auto &e : entries) {
    auto mono = sammine_util::MonomorphizedName::typeclass(
        e.class_name, e.type.to_string(), e.method_name);
    auto key = mono.instance_key();
    if (type_class_instances.contains(key))
      continue;

    TypeClassInstanceInfo info;
    info.class_name = e.class_name;
    info.concrete_types = {e.type};
    info.method_mangled_names[e.method_name] = mono;
    type_class_instances[key] = std::move(info);
  }
}

void BiTypeCheckerVisitor::visit(TypeClassDeclAST *ast) {
  // Already registered in first pass — nothing to do
  register_typeclass_decl(ast);
}

void BiTypeCheckerVisitor::visit(TypeClassInstanceAST *ast) {
  // Registration already done in first pass — just type-check method bodies
  for (auto &method : ast->methods)
    method->accept_vis(this);
}

void BiTypeCheckerVisitor::visit(KernelDefAST *ast) {
  // Kernel defs: CPU visitors see the prototype but don't recurse into kernel body
  enter_new_scope();
  ctx().enclosing_kernel = ast;
  ast->Prototype->accept_vis(this);
  ast->accept_synthesis(this);
  exit_new_scope();
}

// pre order — all empty, logic moved to visit() overrides
void BiTypeCheckerVisitor::preorder_walk(ProgramAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(VarDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(ExternAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(FuncDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(StructDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(EnumDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(TypeAliasDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(PrototypeAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(CallExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(ReturnExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(BinaryExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(StringExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(NumberExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(BoolExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(CharExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(VariableExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(BlockAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(IfExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(UnitExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(TypedVarAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(DerefExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(AddrOfExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(AllocExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(FreeExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(ArrayLiteralExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(RangeExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(IndexExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(LenExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(DimExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(UnaryNegExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(StructLiteralExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(FieldAccessExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(CaseExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(WhileExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(TupleLiteralExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(TypeClassDeclAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(TypeClassInstanceAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(KernelDefAST *ast) {}

// post order — all empty, logic moved to visit() overrides
void BiTypeCheckerVisitor::postorder_walk(ProgramAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(VarDefAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(ExternAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(StructDefAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(EnumDefAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(TypeAliasDefAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(FuncDefAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(PrototypeAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(CallExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(ReturnExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(BinaryExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(StringExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(NumberExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(BoolExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(CharExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(VariableExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(BlockAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(IfExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(UnitExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(TypedVarAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(DerefExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(AddrOfExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(AllocExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(FreeExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(ArrayLiteralExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(RangeExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(IndexExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(LenExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(DimExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(UnaryNegExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(StructLiteralExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(FieldAccessExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(CaseExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(WhileExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(TupleLiteralExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(TypeClassDeclAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(TypeClassInstanceAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(KernelDefAST *ast) {}

} // namespace sammine_lang::AST
