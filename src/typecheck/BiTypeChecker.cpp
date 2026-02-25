#include "typecheck/BiTypeChecker.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "semantics/GeneralSemanticsVisitor.h"
#include "typecheck/Monomorphizer.h"
#include "typecheck/Types.h"
#include "util/LexicalContext.h"
#include "util/Utilities.h"

#define DEBUG_TYPE "typecheck"
#include "util/Logging.h"

//! \file BiTypeChecker.cpp
//! \brief Implementation of BiTypeCheckerVisitor, an ASTVisitor that
//!        traverses the AST to synthesize node types, perform bidirectional
//!        consistency checks, and register functions and variables.
namespace sammine_lang::AST {
// visit overrides — explicit traversal order

void BiTypeCheckerVisitor::visit(ProgramAST *ast) {
  top_level_ast = ast;

  // First pass: register structs, typeclass declarations, and instances
  // so method bodies can reference any type/instance regardless of order.
  for (auto &def : ast->DefinitionVec) {
    if (auto *sd = dynamic_cast<StructDefAST *>(def.get()))
      sd->accept_vis(this);
    else if (auto *tc = dynamic_cast<TypeClassDeclAST *>(def.get()))
      register_typeclass_decl(tc);
    else if (auto *tci = dynamic_cast<TypeClassInstanceAST *>(def.get()))
      register_typeclass_instance(tci);
  }

  // Second pass: full type checking of everything
  for (auto &def : ast->DefinitionVec)
    def->accept_vis(this);
}

void BiTypeCheckerVisitor::visit(FuncDefAST *ast) {
  enter_new_scope();
  id_to_type.top().setScope(ast);
  typename_to_type.top().setScope(ast);

  if (ast->Prototype->is_var_arg) {
    this->add_error(
        ast->Prototype->get_location(),
        "Variadic arguments ('...') are only supported on extern declarations "
        "for C interop, not on function definitions");
  }

  bool is_generic = discover_type_params(ast->Prototype.get());
  ast->accept_synthesis(this);

  if (is_generic) {
    generic_func_defs[ast->Prototype->functionName] = ast;
  } else {
    ast->Block->accept_vis(this);
  }

  exit_new_scope();
}

void BiTypeCheckerVisitor::visit(PrototypeAST *ast) {
  ast->accept_synthesis(this);
  for (auto &var : ast->parameterVectors)
    var->accept_vis(this);
  id_to_type.parent_scope()->registerNameT(ast->functionName, ast->type);
  if (ast->functionName == "main") {
    auto fn_type = std::get<FunctionType>(ast->type.type_data);
    auto return_type = fn_type.get_return_type();
    if (return_type != Type::I32_t()) {
      this->add_error(ast->get_location(),
                      fmt::format("main must return i32, found {}",
                                  return_type.to_string()));
    }
  }
}

void BiTypeCheckerVisitor::visit(VarDefAST *ast) {
  // Special case: array type annotation + array literal RHS
  // Check each element against the declared element type before synthesis
  // runs its own consistency check.
  if (ast->TypedVar->type_expr != nullptr) {
    auto *arr_lit = dynamic_cast<ArrayLiteralExprAST *>(ast->Expression.get());
    if (arr_lit) {
      ast->accept_synthesis(this);
      auto to = ast->type;
      if (to.type_kind == TypeKind::Array) {
        auto expected_elem = std::get<ArrayType>(to.type_data).get_element();
        // Visit each element to type-check sub-expressions
        for (auto &elem : arr_lit->elements)
          elem->accept_vis(this);
        // Check each element against declared element type
        size_t first_error_idx = 0;
        size_t error_count = 0;
        for (size_t i = 0; i < arr_lit->elements.size(); i++) {
          auto elem_type = arr_lit->elements[i]->accept_synthesis(this);
          if (!type_map_ordering.compatible_to_from(expected_elem, elem_type)) {
            if (error_count == 0)
              first_error_idx = i;
            error_count++;
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
            msgs.push_back(fmt::format(
                "{} more type mismatch {} in this array", error_count - 1,
                (error_count - 1 == 1) ? "error" : "errors"));
          this->add_error(arr_lit->elements[first_error_idx]->get_location(),
                          msgs);
          ast->type = Type::Poisoned();
          arr_lit->type = Type::Poisoned();
          return;
        }
        // Set the array literal's type to the declared type
        arr_lit->type = to;
        return;
      }
    }
  }

  // Normal case
  ast->Expression->accept_vis(this);
  ast->accept_synthesis(this);
  auto to = ast->type;
  auto from = ast->Expression->accept_synthesis(this);
  if (to == Type::Poisoned() || from == Type::Poisoned()) {
    ast->type = Type::Poisoned();
  } else if (!type_map_ordering.compatible_to_from(to, from)) {
    this->add_error(ast->Expression->get_location(),
                    fmt::format("Type mismatch: expression has type {}, "
                                "but variable declared as {}",
                                from.to_string(), to.to_string()));
    ast->type = Type::Poisoned();
  }
}

void BiTypeCheckerVisitor::visit(ExternAST *ast) {
  enter_new_scope();
  bool is_generic = discover_type_params(ast->Prototype.get());
  if (is_generic) {
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
  // Skip if already registered (e.g. by first pass)
  if (ast->type.type_kind != TypeKind::NonExistent)
    return;

  std::vector<std::string> field_names;
  std::vector<Type> field_types;
  bool had_error = false;

  for (auto &member : ast->struct_members) {
    auto ft = resolve_type_expr(member->type_expr.get());
    if (ft.type_kind == TypeKind::Poisoned)
      had_error = true;
    field_names.push_back(member->name);
    field_types.push_back(ft);
  }

  if (had_error) {
    ast->type = Type::Poisoned();
    return;
  }

  auto struct_type =
      Type::Struct(ast->struct_name, std::move(field_names),
                   std::move(field_types));
  ast->type = struct_type;
  typename_to_type.registerNameT(ast->struct_name, struct_type);
}

void BiTypeCheckerVisitor::visit(CallExprAST *ast) {
  ast->accept_synthesis(this);
  for (auto &arg : ast->arguments)
    arg->accept_vis(this);

  // If this is a generic call (successful or failed), skip normal arg checking
  if (generic_func_defs.contains(ast->functionName.mangled())) {
    // Trigger monomorphization only if synthesis succeeded
    if (ast->resolved_generic_name.has_value()) {
      auto &mangled = ast->resolved_generic_name.value();
      if (!instantiated_functions.contains(mangled)) {
        auto *generic_def = generic_func_defs[ast->functionName.mangled()];
        auto cloned = Monomorphizer::instantiate(generic_def, mangled,
                                                 ast->type_bindings);

        // Run GeneralSemantics on the cloned def (for implicit return wrapping)
        auto sem = GeneralSemanticsVisitor();
        cloned->accept_vis(&sem);

        // Type-check the cloned def
        cloned->accept_vis(this);

        instantiated_functions.insert(mangled);
        monomorphized_defs.push_back(std::move(cloned));
      }
      // Rewrite call to use mangled name
      ast->functionName = sammine_util::QualifiedName::local(mangled);
    }
    return;
  }

  // Type class calls are fully resolved during synthesis — just rewrite the
  // call target to the mangled instance method name.
  if (ast->is_typeclass_call) {
    if (ast->resolved_generic_name.has_value())
      ast->functionName =
          sammine_util::QualifiedName::local(ast->resolved_generic_name.value());
    return;
  }

  auto ty = try_get_callee_type(ast->functionName.mangled());
  if (!ty.has_value()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Function '{}' not found",
                    ast->functionName.display()));
    return;
  }
  if (ty->type_kind != TypeKind::Function) {
    this->add_error(ast->get_location(),
                    fmt::format("'{}' is not a function",
                                ast->functionName.display()));
    return;
  }
  ast->callee_func_type = ty;
  auto func = std::get<FunctionType>(ty->type_data);
  auto params = func.get_params_types();

  // Skip arg count/type checking for variadic functions
  if (func.is_var_arg())
    return;

  LOG({
    fmt::print(stderr,
               "[typecheck] visit CallExprAST: '{}' callee_func_type = {}, "
               "partial = {}\n",
               ast->functionName.display(), ty->to_string(),
               ast->arguments.size() < params.size() ? "true" : "false");
  });

  if (ast->arguments.size() > params.size()) {
    this->add_error(ast->get_location(),
                    fmt::format("Function '{}' expects {} arguments, got {}",
                                ast->functionName.display(), params.size(),
                                ast->arguments.size()));
    return;
  }

  // Partial application: fewer args than params
  if (ast->arguments.size() < params.size()) {
    ast->is_partial = true;
  }

  // Type-check the provided args against the first N params
  for (size_t i = 0; i < ast->arguments.size(); i++) {
    if (!this->type_map_ordering.compatible_to_from(
            params[i], ast->arguments[i]->type)) {
      this->add_error(ast->arguments[i]->get_location(),
                      fmt::format("Argument {} to '{}': expected {}, got {}",
                                  i + 1, ast->functionName.display(),
                                  params[i].to_string(),
                                  ast->arguments[i]->type.to_string()));
    }
  }
}

void BiTypeCheckerVisitor::visit(ReturnExprAST *ast) {
  if (ast->return_expr)
    ast->return_expr->accept_vis(this);
  ast->accept_synthesis(this);
  auto t = ast->return_expr->accept_synthesis(this);
  auto scope_fn = this->id_to_type.top().s.value();
  auto fn_type = std::get<FunctionType>(scope_fn->type.type_data);
  auto return_type = fn_type.get_return_type();
  if (t != return_type) {
    this->add_error(ast->get_location(),
                    fmt::format("Wrong return type for function {}, expected "
                                "{} but got {}",
                                scope_fn->getFunctionName(),
                                return_type.to_string(), t.to_string()));
  }
}

void BiTypeCheckerVisitor::visit(BinaryExprAST *ast) {
  ast->LHS->accept_vis(this);
  ast->RHS->accept_vis(this);
  ast->type = ast->accept_synthesis(this);
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

void BiTypeCheckerVisitor::visit(IndexExprAST *ast) {
  ast->array_expr->accept_vis(this);
  ast->index_expr->accept_vis(this);
  ast->accept_synthesis(this);
}

void BiTypeCheckerVisitor::visit(LenExprAST *ast) {
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

void BiTypeCheckerVisitor::register_typeclass_decl(TypeClassDeclAST *ast) {
  if (type_class_defs.contains(ast->class_name))
    return;

  TypeClassInfo info;
  info.name = ast->class_name;
  info.type_param = ast->type_param;

  enter_new_scope();
  typename_to_type.registerNameT(ast->type_param,
                                 Type::TypeParam(ast->type_param));

  for (auto &proto : ast->methods) {
    proto->accept_synthesis(this);
    info.methods.push_back(proto.get());
    method_to_class[proto->functionName] = ast->class_name;
  }

  exit_new_scope();
  type_class_defs[ast->class_name] = std::move(info);
}

void BiTypeCheckerVisitor::register_typeclass_instance(
    TypeClassInstanceAST *ast) {
  ast->concrete_type = resolve_type_expr(ast->concrete_type_expr.get());
  if (ast->concrete_type.type_kind == TypeKind::Poisoned)
    return;

  auto it = type_class_defs.find(ast->class_name);
  if (it == type_class_defs.end()) {
    this->add_error(ast->get_location(),
                    fmt::format("Unknown type class '{}'", ast->class_name));
    return;
  }

  TypeClassInstanceInfo inst_info;
  inst_info.class_name = ast->class_name;
  inst_info.concrete_type = ast->concrete_type;

  for (auto &method : ast->methods) {
    std::string original_name = method->Prototype->functionName;
    std::string mangled = ast->class_name + "$" +
                          ast->concrete_type.to_string() + "$" + original_name;
    method->Prototype->functionName = mangled;
    inst_info.method_mangled_names[original_name] = mangled;
  }

  std::string key =
      ast->class_name + "$" + ast->concrete_type.to_string();
  type_class_instances[key] = std::move(inst_info);
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

// pre order — all empty, logic moved to visit() overrides
void BiTypeCheckerVisitor::preorder_walk(ProgramAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(VarDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(ExternAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(FuncDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(StructDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(PrototypeAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(CallExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(ReturnExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(BinaryExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(StringExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(NumberExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(BoolExprAST *ast) {}
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
void BiTypeCheckerVisitor::preorder_walk(IndexExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(LenExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(UnaryNegExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(StructLiteralExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(FieldAccessExprAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(TypeClassDeclAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(TypeClassInstanceAST *ast) {}

// post order — all empty, logic moved to visit() overrides
void BiTypeCheckerVisitor::postorder_walk(ProgramAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(VarDefAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(ExternAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(StructDefAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(FuncDefAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(PrototypeAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(CallExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(ReturnExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(BinaryExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(StringExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(NumberExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(BoolExprAST *ast) {}
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
void BiTypeCheckerVisitor::postorder_walk(IndexExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(LenExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(UnaryNegExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(StructLiteralExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(FieldAccessExprAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(TypeClassDeclAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(TypeClassInstanceAST *ast) {}

Type BiTypeCheckerVisitor::synthesize(ProgramAST *ast) {
  return Type::NonExistent();
}

Type BiTypeCheckerVisitor::synthesize(VarDefAST *ast) {
  if (ast->synthesized())
    return ast->type;

  // if you dont have type lexeme for typed var, then just assign type of expr
  // to typed var, if we dont have expr also, then we add error
  //
  // if you do, then just use type lexeme as type of typed var

  if (ast->TypedVar->type_expr != nullptr)
    ast->type = ast->TypedVar->accept_synthesis(this);
  else if (ast->Expression)
    ast->type = ast->Expression->accept_synthesis(this);
  else {
    this->add_error(ast->get_location(),
                    "Variable declared without initializer");
    ast->type = Type::Poisoned();
  }

  ast->type.is_mutable = ast->is_mutable;
  id_to_type.registerNameT(ast->TypedVar->name, ast->type);
  LOG({
    fmt::print(stderr, "[typecheck] synthesize VarDefAST: '{}' : {} ({})\n",
               ast->TypedVar->name, ast->type.to_string(),
               ast->is_mutable ? "mutable" : "immutable");
  });
  return ast->type;
}

Type BiTypeCheckerVisitor::synthesize(ExternAST *ast) {
  return Type::NonExistent();
}
Type BiTypeCheckerVisitor::synthesize(StructDefAST *ast) {
  return Type::NonExistent();
}
Type BiTypeCheckerVisitor::synthesize(FuncDefAST *ast) {
  if (ast->synthesized())
    return ast->type;

  return ast->type = ast->Prototype->accept_synthesis(this);
}

Type BiTypeCheckerVisitor::synthesize(PrototypeAST *ast) {
  auto v = std::vector<Type>();
  for (size_t i = 0; i < ast->parameterVectors.size(); i++)
    v.push_back(ast->parameterVectors[i]->accept_synthesis(this));

  if (ast->returnsUnit())
    v.push_back(Type::Unit());
  else
    v.push_back(resolve_type_expr(ast->return_type_expr.get()));
  ast->type = Type::Function(std::move(v), ast->is_var_arg);

  LOG({
    fmt::print(stderr, "[typecheck] synthesize PrototypeAST: '{}' -> {}\n",
               ast->functionName, ast->type.to_string());
  });
  return ast->type;
}

Type BiTypeCheckerVisitor::synthesize(CallExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  // --- Type class method dispatch ---
  if (!ast->explicit_type_args.empty()) {
    auto method_name = ast->functionName.mangled();
    auto class_it = method_to_class.find(method_name);
    if (class_it != method_to_class.end()) {
      auto &class_name = class_it->second;
      auto &tc = type_class_defs[class_name];

      // Resolve the explicit type argument
      Type concrete = resolve_type_expr(ast->explicit_type_args[0].get());
      if (concrete.type_kind == TypeKind::Poisoned)
        return ast->type = Type::Poisoned();

      // Find the instance
      std::string key = class_name + "$" + concrete.to_string();
      auto inst_it = type_class_instances.find(key);
      if (inst_it == type_class_instances.end()) {
        add_error(ast->get_location(),
                  fmt::format("No instance of {}<{}>", class_name,
                              concrete.to_string()));
        return ast->type = Type::Poisoned();
      }

      // Find the method prototype in the type class
      PrototypeAST *method_proto = nullptr;
      for (auto *p : tc.methods) {
        if (p->functionName == method_name) {
          method_proto = p;
          break;
        }
      }
      if (!method_proto) {
        add_error(ast->get_location(),
                  fmt::format("Method '{}' not found in type class '{}'",
                              method_name, class_name));
        return ast->type = Type::Poisoned();
      }

      // Validate argument count
      auto func_type = std::get<FunctionType>(method_proto->type.type_data);
      auto params = func_type.get_params_types();
      if (ast->arguments.size() != params.size()) {
        add_error(ast->get_location(),
                  fmt::format("Type class method '{}' expects {} arguments, "
                              "got {}",
                              method_name, params.size(),
                              ast->arguments.size()));
        return ast->type = Type::Poisoned();
      }

      // Set resolved call target
      auto &mangled =
          inst_it->second.method_mangled_names[method_name];
      ast->resolved_generic_name = mangled;
      ast->is_typeclass_call = true;

      // Return type: substitute type param with concrete type
      auto return_type = func_type.get_return_type();
      std::unordered_map<std::string, Type> bindings;
      bindings[tc.type_param] = concrete;
      return_type = substitute(return_type, bindings);

      return ast->type = return_type;
    }
  }

  // Check if this is a generic function call
  auto gen_it = generic_func_defs.find(ast->functionName.mangled());
  if (gen_it != generic_func_defs.end()) {
    auto *generic_def = gen_it->second;
    auto generic_type = generic_def->type;
    auto func = std::get<FunctionType>(generic_type.type_data);
    auto params = func.get_params_types();

    if (ast->arguments.size() != params.size()) {
      this->add_error(
          ast->get_location(),
          fmt::format("Generic function '{}' expects {} arguments, got {}",
                      ast->functionName.display(), params.size(), ast->arguments.size()));
      return ast->type = Type::Poisoned();
    }

    std::unordered_map<std::string, Type> bindings;

    if (!ast->explicit_type_args.empty()) {
      // Explicit type argument: f<T>(args)
      auto &type_params = generic_def->Prototype->type_params;
      assert(type_params.size() == 1 && "Only single type parameter supported");

      Type resolved = resolve_type_expr(ast->explicit_type_args[0].get());
      if (resolved.type_kind == TypeKind::Poisoned)
        return ast->type = Type::Poisoned();
      bindings[type_params[0]] = resolved;

      // Synthesize and check argument types against the now-known param types
      for (size_t i = 0; i < ast->arguments.size(); i++) {
        auto arg_type = ast->arguments[i]->accept_synthesis(this);
        if (arg_type.type_kind == TypeKind::Poisoned)
          return ast->type = Type::Poisoned();
        auto expected = substitute(params[i], bindings);
        if (!type_map_ordering.compatible_to_from(expected, arg_type)) {
          this->add_error(ast->arguments[i]->get_location(),
                          fmt::format("Type mismatch in argument {} of '{}': "
                                      "expected {}, got {}",
                                      i + 1, ast->functionName.display(),
                                      expected.to_string(),
                                      arg_type.to_string()));
          return ast->type = Type::Poisoned();
        }
      }
    } else {
      // Infer type arguments from call arguments
      for (size_t i = 0; i < ast->arguments.size(); i++) {
        auto arg_type = ast->arguments[i]->accept_synthesis(this);
        if (arg_type.type_kind == TypeKind::Poisoned)
          return ast->type = Type::Poisoned();
        if (!unify(params[i], arg_type, bindings)) {
          auto expected = substitute(params[i], bindings);
          this->add_error(ast->arguments[i]->get_location(),
                          fmt::format("Type mismatch in argument {} of '{}': "
                                      "expected {}, got {}",
                                      i + 1, ast->functionName.display(),
                                      expected.to_string(),
                                      arg_type.to_string()));
          return ast->type = Type::Poisoned();
        }
      }

      // Check all type params resolved
      for (auto &tp : generic_def->Prototype->type_params) {
        if (bindings.find(tp) == bindings.end()) {
          this->add_error(
              ast->get_location(),
              fmt::format(
                  "Type parameter '{}' could not be inferred for '{}'",
                  tp, ast->functionName.display()));
          return ast->type = Type::Poisoned();
        }
      }
    }

    // Compute mangled name
    std::string mangled = ast->functionName.mangled();
    for (auto &tp : generic_def->Prototype->type_params)
      mangled += "." + bindings[tp].to_string();

    // Substitute return type
    auto ret_type = substitute(func.get_return_type(), bindings);
    ast->resolved_generic_name = mangled;
    ast->type_bindings = bindings;
    ast->callee_func_type = generic_type;

    return ast->type = ret_type;
  }

  // Search current scope + parent scopes for the callee
  auto ty = try_get_callee_type(ast->functionName.mangled());
  if (!ty.has_value()) {
    this->add_error(ast->get_location(),
                    fmt::format("Function '{}' not found",
                                ast->functionName.display()));
    return ast->type = Type::Poisoned();
  }

  ast->callee_func_type = ty;

  LOG({
    fmt::print(stderr,
               "[typecheck] synthesize CallExprAST: '{}' callee type: {}\n",
               ast->functionName.display(), ty->to_string());
  });

  switch (ty->type_kind) {
  case TypeKind::Function: {
    auto func = std::get<FunctionType>(ty->type_data);
    auto params = func.get_params_types();

    // Variadic functions: accept any number of args >= fixed params
    if (func.is_var_arg()) {
      if (ast->arguments.size() < params.size()) {
        this->add_error(
            ast->get_location(),
            fmt::format("Variadic function '{}' requires at least {} "
                        "arguments, got {}",
                        ast->functionName.display(), params.size(),
                        ast->arguments.size()));
        return ast->type = Type::Poisoned();
      }
      return ast->type = func.get_return_type();
    }

    // Detect partial application: fewer args than params
    if (ast->arguments.size() < params.size()) {
      ast->is_partial = true;
      std::vector<Type> remaining;
      for (size_t i = ast->arguments.size(); i < params.size(); i++)
        remaining.push_back(params[i]);
      remaining.push_back(func.get_return_type());
      auto result = Type::Function(std::move(remaining));
      LOG({
        fmt::print(stderr,
                   "[typecheck] synthesize CallExprAST: '{}' partial ({} of {} "
                   "args) -> {}\n",
                   ast->functionName.display(), ast->arguments.size(), params.size(),
                   result.to_string());
      });
      return ast->type = result;
    }

    // Full call: return the function's return type
    LOG({
      fmt::print(stderr,
                 "[typecheck] synthesize CallExprAST: '{}' full call -> {}\n",
                 ast->functionName.display(), func.get_return_type().to_string());
    });
    return ast->type = func.get_return_type();
  }
  case TypeKind::String:
    this->add_error(ast->get_location(),
                    "A string cannot be in place of a call expression");
    return Type::Poisoned();
  case TypeKind::I32_t:
  case TypeKind::I64_t:
  case TypeKind::F64_t:
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::Pointer:
  case TypeKind::Array:
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Struct:
  case TypeKind::Poisoned:
  case TypeKind::Integer:
  case TypeKind::Flt:
  case TypeKind::TypeParam:
    this->abort(fmt::format("should not happen here with function {}",
                            ast->functionName.display()));
  }
}

Type BiTypeCheckerVisitor::synthesize(ReturnExprAST *ast) {
  return ast->type = Type::Never();
}
Type BiTypeCheckerVisitor::synthesize(BinaryExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  // If either operand is Never, the whole expression is Never
  // (control flow diverges before the binary operation is evaluated)
  if (ast->LHS->type.type_kind == TypeKind::Never ||
      ast->RHS->type.type_kind == TypeKind::Never) {
    return ast->type = Type::Never();
  }

  if (!this->type_map_ordering.compatible_to_from(ast->LHS->type,
                                                  ast->RHS->type)) {
    this->add_error(
        ast->Op->get_location(),
        fmt::format("Incompatible types for operator '{}': {} and {}",
                    ast->Op->lexeme, ast->LHS->type.to_string(),
                    ast->RHS->type.to_string()));
    return ast->type = ast->LHS->type;
  }
  if (ast->Op->is_comparison()) {
    auto kind = ast->LHS->type.type_kind;
    if (kind == TypeKind::Array || kind == TypeKind::Pointer) {
      if (ast->Op->tok_type != TokEQUAL && ast->Op->tok_type != TokNOTEqual) {
        this->add_error(
            ast->Op->get_location(),
            fmt::format("Only == and != are supported for {} types",
                        ast->LHS->type.to_string()));
        return ast->type = Type::Poisoned();
      }
    }
    return ast->type = Type::Bool();
  }
  if (ast->Op->is_assign()) {
    if (auto *var = dynamic_cast<VariableExprAST *>(ast->LHS.get())) {
      if (!var->type.is_mutable) {
        this->add_error(
            ast->Op->get_location(),
            fmt::format("Cannot reassign immutable variable '{}'. "
                        "Use 'let mut' or 'mut' to declare it as mutable",
                        var->variableName));
      }
    }
    return ast->type = Type::Unit();
  }

  return ast->type = ast->LHS->type;
}

Type BiTypeCheckerVisitor::synthesize(StringExprAST *ast) {
  if (ast->synthesized())
    return ast->type;
  return ast->type = Type::String(ast->string_content);
}

Type BiTypeCheckerVisitor::synthesize(NumberExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

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
      ast->type = Type::I32_t();
    else if (suffix == "i64")
      ast->type = Type::I64_t();
    else if (suffix == "f64")
      ast->type = Type::F64_t();
    else
      this->abort_on(
          true,
          fmt::format("invalid type suffix '{}' on number literal", suffix));
  } else if (ast->number.find('.') == std::string::npos)
    ast->type = Type::I32_t();
  else
    ast->type = Type::F64_t();

  return ast->type;
}
Type BiTypeCheckerVisitor::synthesize(BoolExprAST *ast) {
  return ast->type = Type::Bool();
}
Type BiTypeCheckerVisitor::synthesize(VariableExprAST *ast) {
  ast->type = id_to_type.recursive_get_from_name(ast->variableName);
  LOG({
    fmt::print(stderr, "[typecheck] synthesize VariableExprAST: '{}' -> {}\n",
               ast->variableName, ast->type.to_string());
  });
  return ast->type;
}
Type BiTypeCheckerVisitor::synthesize(BlockAST *ast) {
  // Block typing rule:
  // 1. Type each statement in order
  // 2. If any statement has type ! (Never) and is not the return , stop: the
  // block's type is !
  // 3. Otherwise, the block's type is the type of the last expression
  // 4. If there is no final expression, the type is ()

  if (ast->Statements.empty()) {
    return ast->type = Type::Unit();
  }

  for (auto &stmt : ast->Statements) {
    auto stmt_type = stmt->accept_synthesis(this);
    if (stmt_type.type_kind == TypeKind::Never) {
      return ast->type = Type::Never();
    }
  }

  // Block's type is the type of the last expression
  return ast->type = ast->Statements.back()->type;
}
Type BiTypeCheckerVisitor::synthesize(UnitExprAST *ast) {
  if (ast->synthesized())
    return ast->type;
  return ast->type = Type::Unit();
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
    return ast->type = Type::Never();
  }

  // If one branch has type Never, the if has the type of the other branch
  if (then_type.type_kind == TypeKind::Never) {
    return ast->type = else_type;
  }
  if (else_type.type_kind == TypeKind::Never) {
    return ast->type = then_type;
  }

  // Both branches must have compatible types
  if (then_type != else_type) {
    this->add_error(
        ast->elseBlockAST->get_location(),
        fmt::format("If branches have incompatible types: then has {}, else "
                    "has {}",
                    then_type.to_string(), else_type.to_string()));
    return ast->type = Type::Poisoned();
  }

  return ast->type = then_type;
}
Type BiTypeCheckerVisitor::synthesize(TypedVarAST *ast) {
  if (ast->synthesized())
    return ast->type;
  auto ast_type = resolve_type_expr(ast->type_expr.get());

  ast_type.is_mutable = ast->is_mutable;
  id_to_type.registerNameT(ast->name, ast_type);
  return ast->type = ast_type;
}

Type BiTypeCheckerVisitor::synthesize(DerefExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::Pointer) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot dereference non-pointer type '{}'",
                                operand_type.to_string()));
    return ast->type = Type::Poisoned();
  }
  return ast->type =
             std::get<PointerType>(operand_type.type_data).get_pointee();
}

Type BiTypeCheckerVisitor::synthesize(AddrOfExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  auto operand_type = ast->operand->accept_synthesis(this);
  return ast->type = Type::Pointer(operand_type);
}

Type BiTypeCheckerVisitor::synthesize(AllocExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  // Resolve the element type from alloc<T>
  auto element_type = resolve_type_expr(ast->type_arg.get());
  if (element_type.type_kind == TypeKind::Poisoned)
    return ast->type = Type::Poisoned();

  // The count operand must be an integer
  auto count_type = ast->operand->accept_synthesis(this);
  if (count_type.type_kind != TypeKind::I32_t &&
      count_type.type_kind != TypeKind::I64_t &&
      count_type.type_kind != TypeKind::Integer) {
    this->add_error(ast->operand->get_location(),
                    fmt::format("alloc count must be an integer, got '{}'",
                                count_type.to_string()));
    return ast->type = Type::Poisoned();
  }

  return ast->type = Type::Pointer(element_type);
}

Type BiTypeCheckerVisitor::synthesize(FreeExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::Pointer) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot free non-pointer type '{}'",
                                operand_type.to_string()));
    return ast->type = Type::Poisoned();
  }
  return ast->type = Type::Unit();
}

Type BiTypeCheckerVisitor::synthesize(ArrayLiteralExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  if (ast->elements.empty()) {
    this->add_error(ast->get_location(),
                    "Array literal must have at least one element");
    return ast->type = Type::Poisoned();
  }

  auto first_type = ast->elements[0]->accept_synthesis(this);
  for (size_t i = 1; i < ast->elements.size(); i++) {
    auto elem_type = ast->elements[i]->accept_synthesis(this);
    if (elem_type != first_type) {
      this->add_error(ast->elements[i]->get_location(),
                      fmt::format("Array element {}: expected {}, got {}", i,
                                  first_type.to_string(),
                                  elem_type.to_string()));
      return ast->type = Type::Poisoned();
    }
  }

  return ast->type = Type::Array(first_type, ast->elements.size());
}
Type BiTypeCheckerVisitor::synthesize(IndexExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  auto arr_type = ast->array_expr->accept_synthesis(this);
  if (arr_type.type_kind != TypeKind::Array) {
    this->add_error(
        ast->get_location(),
        fmt::format("Cannot index non-array type '{}'", arr_type.to_string()));
    return ast->type = Type::Poisoned();
  }

  auto idx_type = ast->index_expr->accept_synthesis(this);
  if (idx_type.type_kind != TypeKind::I32_t &&
      idx_type.type_kind != TypeKind::I64_t) {
    this->add_error(ast->get_location(),
                    fmt::format("Array index must be integer, got '{}'",
                                idx_type.to_string()));
    return ast->type = Type::Poisoned();
  }

  auto &arr_data = std::get<ArrayType>(arr_type.type_data);
  if (auto *num = dynamic_cast<NumberExprAST *>(ast->index_expr.get())) {
    int idx = std::stoi(num->number);
    int size = static_cast<int>(arr_data.get_size());
    if (idx < 0 || idx >= size) {
      this->add_error(
          ast->get_location(),
          fmt::format("Array index out of bounds: index {} on array of size {}",
                      idx, size));
      return ast->type = Type::Poisoned();
    }
  }

  return ast->type = arr_data.get_element();
}
Type BiTypeCheckerVisitor::synthesize(LenExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::Array) {
    this->add_error(ast->get_location(),
                    fmt::format("len() requires array type, got '{}'",
                                operand_type.to_string()));
    return ast->type = Type::Poisoned();
  }

  return ast->type = Type::I32_t();
}

Type BiTypeCheckerVisitor::synthesize(UnaryNegExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  auto operand_type = ast->operand->accept_synthesis(this);
  if (operand_type.type_kind != TypeKind::I32_t &&
      operand_type.type_kind != TypeKind::I64_t &&
      operand_type.type_kind != TypeKind::F64_t) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot negate non-numeric type '{}'",
                                operand_type.to_string()));
    return ast->type = Type::Poisoned();
  }
  return ast->type = operand_type;
}

Type BiTypeCheckerVisitor::synthesize(StructLiteralExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  if (ast->struct_name.is_unresolved()) {
    this->add_error(ast->get_location(),
                    fmt::format("Module '{}' is not imported",
                                ast->struct_name.module));
    return ast->type = Type::Poisoned();
  }

  auto type_opt = get_typename_type(ast->struct_name.mangled());
  if (!type_opt.has_value()) {
    this->add_error(ast->get_location(),
                    fmt::format("Unknown struct type '{}'", ast->struct_name.display()));
    return ast->type = Type::Poisoned();
  }

  auto struct_type = type_opt.value();
  if (struct_type.type_kind != TypeKind::Struct) {
    this->add_error(
        ast->get_location(),
        fmt::format("'{}' is not a struct type", ast->struct_name.display()));
    return ast->type = Type::Poisoned();
  }

  auto &st = std::get<StructType>(struct_type.type_data);

  // Check field count
  if (ast->field_names.size() != st.field_count()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Struct '{}' expects {} fields, got {}", ast->struct_name.display(),
                    st.field_count(), ast->field_names.size()));
    return ast->type = Type::Poisoned();
  }

  // Check each field
  for (size_t i = 0; i < ast->field_names.size(); i++) {
    auto idx = st.get_field_index(ast->field_names[i]);
    if (!idx.has_value()) {
      this->add_error(
          ast->field_values[i]->get_location(),
          fmt::format("Struct '{}' has no field named '{}'", ast->struct_name.display(),
                      ast->field_names[i]));
      return ast->type = Type::Poisoned();
    }

    auto expected = st.get_field_type(idx.value());
    auto actual = ast->field_values[i]->accept_synthesis(this);
    if (!type_map_ordering.compatible_to_from(expected, actual)) {
      this->add_error(
          ast->field_values[i]->get_location(),
          fmt::format("Field '{}': expected type {}, got {}",
                      ast->field_names[i], expected.to_string(),
                      actual.to_string()));
      return ast->type = Type::Poisoned();
    }
  }

  return ast->type = struct_type;
}

Type BiTypeCheckerVisitor::synthesize(FieldAccessExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  auto obj_type = ast->object_expr->accept_synthesis(this);
  if (obj_type.type_kind == TypeKind::Poisoned)
    return ast->type = Type::Poisoned();

  if (obj_type.type_kind != TypeKind::Struct) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot access field '{}' on non-struct type {}",
                                ast->field_name, obj_type.to_string()));
    return ast->type = Type::Poisoned();
  }

  auto &st = std::get<StructType>(obj_type.type_data);
  auto idx = st.get_field_index(ast->field_name);
  if (!idx.has_value()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Struct '{}' has no field named '{}'", st.get_name(),
                    ast->field_name));
    return ast->type = Type::Poisoned();
  }

  return ast->type = st.get_field_type(idx.value());
}

Type BiTypeCheckerVisitor::synthesize(TypeClassDeclAST *ast) {
  return Type::NonExistent();
}
Type BiTypeCheckerVisitor::synthesize(TypeClassInstanceAST *ast) {
  return Type::NonExistent();
}

// --- Generic support: unification, substitution ---

bool BiTypeCheckerVisitor::contains_type_param(const Type &type,
                                               const std::string &param_name) {
  if (type.type_kind == TypeKind::TypeParam)
    return std::get<std::string>(type.type_data) == param_name;

  if (type.type_kind == TypeKind::Pointer) {
    auto pointee = std::get<PointerType>(type.type_data).get_pointee();
    return contains_type_param(pointee, param_name);
  }
  if (type.type_kind == TypeKind::Array) {
    auto elem = std::get<ArrayType>(type.type_data).get_element();
    return contains_type_param(elem, param_name);
  }
  if (type.type_kind == TypeKind::Function) {
    auto fn = std::get<FunctionType>(type.type_data);
    for (auto &p : fn.get_params_types())
      if (contains_type_param(p, param_name))
        return true;
    return contains_type_param(fn.get_return_type(), param_name);
  }
  return false;
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
  return type;
}

} // namespace sammine_lang::AST
