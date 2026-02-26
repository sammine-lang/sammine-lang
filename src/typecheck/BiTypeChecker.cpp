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

// --- Numeric literal type inference helpers ---

/// Default a polymorphic numeric type to its concrete default:
/// Integer → I32_t, Flt → F64_t. Non-polymorphic types pass through unchanged.
static Type default_polymorphic_type(const Type &t) {
  if (t.type_kind == TypeKind::Integer)
    return Type::I32_t();
  if (t.type_kind == TypeKind::Flt)
    return Type::F64_t();
  return t;
}

/// Recursively resolve polymorphic literal types in an expression tree
/// to a concrete target type. Walks through UnaryNeg, BinaryExpr, IfExpr,
/// and BlockAST to reach all leaf literals.
static void resolve_literal_type(ExprAST *expr, const Type &target) {
  if (!expr || !expr->type.is_polymorphic_numeric())
    return;

  expr->type = target;

  if (auto *unary = dynamic_cast<UnaryNegExprAST *>(expr)) {
    resolve_literal_type(unary->operand.get(), target);
  } else if (auto *binary = dynamic_cast<BinaryExprAST *>(expr)) {
    resolve_literal_type(binary->LHS.get(), target);
    resolve_literal_type(binary->RHS.get(), target);
  } else if (auto *if_expr = dynamic_cast<IfExprAST *>(expr)) {
    if (if_expr->thenBlockAST && !if_expr->thenBlockAST->Statements.empty()) {
      auto *last_then = if_expr->thenBlockAST->Statements.back().get();
      resolve_literal_type(last_then, target);
      if_expr->thenBlockAST->type = target;
    }
    if (if_expr->elseBlockAST && !if_expr->elseBlockAST->Statements.empty()) {
      auto *last_else = if_expr->elseBlockAST->Statements.back().get();
      resolve_literal_type(last_else, target);
      if_expr->elseBlockAST->type = target;
    }
  }
  // For all other expression types (NumberExprAST, CallExprAST, IndexExprAST,
  // etc.), the type is already set above — no children to recurse into.
}

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

  // First pass: register structs, enums, typeclass declarations, and instances
  // so method bodies can reference any type/instance regardless of order.
  for (auto &def : ast->DefinitionVec) {
    if (auto *sd = dynamic_cast<StructDefAST *>(def.get()))
      sd->accept_vis(this);
    else if (auto *ed = dynamic_cast<EnumDefAST *>(def.get()))
      ed->accept_vis(this);
    else if (auto *tc = dynamic_cast<TypeClassDeclAST *>(def.get()))
      register_typeclass_decl(tc);
    else if (auto *tci = dynamic_cast<TypeClassInstanceAST *>(def.get()))
      register_typeclass_instance(tci);
  }

  // Register compiler-builtin operator instances for primitive types
  // (Add/Sub/Mul/Div/Mod on i32, i64, f64, char) — no source bodies,
  // codegen emits inline ops for these.
  register_builtin_op_instances();

  // Second pass: pre-register all function signatures so mutual recursion works
  for (auto &def : ast->DefinitionVec) {
    if (auto *fd = dynamic_cast<FuncDefAST *>(def.get())) {
      if (fd->Prototype->is_generic())
        generic_func_defs[fd->Prototype->functionName.mangled()] = fd;
      else
        pre_register_function(fd->Prototype.get());
    } else if (auto *ext = dynamic_cast<ExternAST *>(def.get())) {
      if (!ext->Prototype->is_generic())
        pre_register_function(ext->Prototype.get());
    } else if (auto *tci = dynamic_cast<TypeClassInstanceAST *>(def.get())) {
      for (auto &method : tci->methods)
        if (!method->Prototype->is_generic())
          pre_register_function(method->Prototype.get());
    }
  }

  // Third pass: full type checking of everything
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
    generic_func_defs[ast->Prototype->functionName.mangled()] = ast;
  } else {
    ast->Block->accept_vis(this);
  }

  exit_new_scope();
}

void BiTypeCheckerVisitor::visit(PrototypeAST *ast) {
  ast->accept_synthesis(this);
  for (auto &var : ast->parameterVectors)
    var->accept_vis(this);
  id_to_type.parent_scope()->registerNameT(ast->functionName.mangled(), ast->type);
  if (ast->functionName.name == "main") {
    auto fn_type = std::get<FunctionType>(ast->type.type_data);
    auto return_type = fn_type.get_return_type();
    if (return_type != Type::I32_t()) {
      this->add_error(ast->get_location(),
                      fmt::format("main must return i32, found {}",
                                  return_type.to_string()));
    }
  }
}

bool BiTypeCheckerVisitor::check_array_literal_against_annotation(
    VarDefAST *ast, ArrayLiteralExprAST *arr_lit, const ArrayType &arr_type) {
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
    ast->type = Type::Poisoned();
    arr_lit->type = Type::Poisoned();
    return true;
  }
  return false;
}

void BiTypeCheckerVisitor::visit(VarDefAST *ast) {
  // Special case: array type annotation + array literal RHS
  if (ast->TypedVar->type_expr != nullptr) {
    if (auto *arr_lit =
            dynamic_cast<ArrayLiteralExprAST *>(ast->Expression.get())) {
      ast->accept_synthesis(this);
      if (ast->type.type_kind == TypeKind::Array) {
        auto &arr_data = std::get<ArrayType>(ast->type.type_data);
        for (auto &elem : arr_lit->elements)
          elem->accept_vis(this);
        if (check_array_literal_against_annotation(ast, arr_lit, arr_data))
          return;
        arr_lit->type = ast->type;
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
      Type::Struct(ast->struct_name.mangled(), std::move(field_names),
                   std::move(field_types));
  ast->type = struct_type;
  typename_to_type.registerNameT(ast->struct_name.mangled(), struct_type);
}

void BiTypeCheckerVisitor::visit(EnumDefAST *ast) {
  std::vector<EnumType::VariantInfo> variant_infos;
  bool had_error = false;

  for (auto &variant : ast->variants) {
    EnumType::VariantInfo info;
    info.name = variant.name;
    for (auto &type_expr : variant.payload_types) {
      auto resolved = resolve_type_expr(type_expr.get());
      if (resolved.type_kind == TypeKind::Poisoned)
        had_error = true;
      info.payload_types.push_back(resolved);
    }
    variant_infos.push_back(std::move(info));
  }

  if (had_error) {
    ast->type = Type::Poisoned();
    return;
  }

  auto enum_type =
      Type::Enum(ast->enum_name, std::move(variant_infos));
  ast->type = enum_type;
  typename_to_type.registerNameT(ast->enum_name.mangled(), enum_type);

  // Register variant constructors (qualified access only: Enum::Variant)
  auto &et = std::get<EnumType>(enum_type.type_data);
  for (size_t i = 0; i < et.variant_count(); i++) {
    auto &vi = et.get_variant(i);
    variant_constructors[vi.name] = {enum_type, i};
  }
}

void BiTypeCheckerVisitor::visit(CallExprAST *ast) {
  ast->accept_synthesis(this);
  for (auto &arg : ast->arguments)
    arg->accept_vis(this);

  // Enum constructors are fully resolved during synthesis
  if (ast->is_enum_constructor)
    return;

  // Generic calls: trigger monomorphization if synthesis succeeded
  if (generic_func_defs.contains(ast->functionName.mangled())) {
    if (ast->resolved_generic_name.has_value()) {
      auto &mangled = ast->resolved_generic_name.value();
      if (!instantiated_functions.contains(mangled)) {
        auto *generic_def = generic_func_defs[ast->functionName.mangled()];
        auto cloned = Monomorphizer::instantiate(generic_def, mangled,
                                                 ast->type_bindings);
        auto sem = GeneralSemanticsVisitor();
        cloned->accept_vis(&sem);
        cloned->accept_vis(this);
        instantiated_functions.insert(mangled);
        monomorphized_defs.push_back(std::move(cloned));
      }
      ast->functionName = sammine_util::QualifiedName::local(mangled);
    }
    return;
  }

  // Typeclass calls: rewrite call target to mangled instance method name
  if (ast->is_typeclass_call) {
    if (ast->resolved_generic_name.has_value())
      ast->functionName =
          sammine_util::QualifiedName::local(ast->resolved_generic_name.value());
    return;
  }

  // Normal calls: check arg types now that args have been visited
  if (!ast->callee_func_type.has_value() ||
      ast->callee_func_type->type_kind != TypeKind::Function)
    return;

  auto func = std::get<FunctionType>(ast->callee_func_type->type_data);
  if (func.is_var_arg())
    return;

  auto params = func.get_params_types();
  for (size_t i = 0; i < ast->arguments.size(); i++) {
    if (!this->type_map_ordering.compatible_to_from(
            params[i], ast->arguments[i]->type)) {
      this->add_error(ast->arguments[i]->get_location(),
                      fmt::format("Argument {} to '{}': expected {}, got {}",
                                  i + 1, ast->functionName.display(),
                                  params[i].to_string(),
                                  ast->arguments[i]->type.to_string()));
    } else if (ast->arguments[i]->type.is_polymorphic_numeric()) {
      resolve_literal_type(ast->arguments[i].get(), params[i]);
    }
  }
}

void BiTypeCheckerVisitor::visit(ReturnExprAST *ast) {
  // Special case: array literal return — propagate function return type
  // into array elements before they default to i32.
  if (auto *arr_lit =
          dynamic_cast<ArrayLiteralExprAST *>(ast->return_expr.get())) {
    auto scope_fn = this->id_to_type.top().s.value();
    auto fn_type = std::get<FunctionType>(scope_fn->type.type_data);
    auto return_type = fn_type.get_return_type();
    if (return_type.type_kind == TypeKind::Array) {
      auto &arr_data = std::get<ArrayType>(return_type.type_data);
      for (auto &elem : arr_lit->elements)
        elem->accept_vis(this);
      auto expected_elem = arr_data.get_element();
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
        msgs.push_back(fmt::format(
            "Array element type mismatch: expected {}, got {}",
            expected_elem.to_string(),
            arr_lit->elements[first_error_idx]
                ->accept_synthesis(this)
                .to_string()));
        if (error_count > 1)
          msgs.push_back(
              fmt::format("{} more type mismatch {} in this array",
                          error_count - 1,
                          (error_count - 1 == 1) ? "error" : "errors"));
        this->add_error(arr_lit->elements[first_error_idx]->get_location(),
                        msgs);
      }
      arr_lit->type = return_type;
      ast->accept_synthesis(this);
      return;
    }
  }

  // Normal case
  if (ast->return_expr)
    ast->return_expr->accept_vis(this);
  ast->accept_synthesis(this);
  auto t = ast->return_expr->accept_synthesis(this);
  auto scope_fn = this->id_to_type.top().s.value();
  auto fn_type = std::get<FunctionType>(scope_fn->type.type_data);
  auto return_type = fn_type.get_return_type();
  if (!type_map_ordering.compatible_to_from(return_type, t)) {
    this->add_error(ast->get_location(),
                    fmt::format("Wrong return type for function {}, expected "
                                "{} but got {}",
                                scope_fn->getFunctionName(),
                                return_type.to_string(), t.to_string()));
  } else if (t.is_polymorphic_numeric()) {
    resolve_literal_type(ast->return_expr.get(), return_type);
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
    method_to_class[proto->functionName.mangled()] = ast->class_name;
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
    std::string original_name = method->Prototype->functionName.name;
    std::string mangled = ast->class_name + "$" +
                          ast->concrete_type.to_string() + "$" + original_name;
    method->Prototype->functionName = sammine_util::QualifiedName::local(mangled);
    inst_info.method_mangled_names[original_name] = mangled;
  }

  std::string key =
      ast->class_name + "$" + ast->concrete_type.to_string();
  type_class_instances[key] = std::move(inst_info);
}

void BiTypeCheckerVisitor::register_builtin_op_instances() {
  struct BuiltinEntry {
    const char *class_name;
    const char *method_name;
    Type type;
  };

  static const BuiltinEntry entries[] = {
      {"Add", "add", Type::I32_t()}, {"Add", "add", Type::I64_t()},
      {"Add", "add", Type::F64_t()}, {"Add", "add", Type::Char()},
      {"Sub", "sub", Type::I32_t()}, {"Sub", "sub", Type::I64_t()},
      {"Sub", "sub", Type::F64_t()}, {"Mul", "mul", Type::I32_t()},
      {"Mul", "mul", Type::I64_t()}, {"Mul", "mul", Type::F64_t()},
      {"Div", "div", Type::I32_t()}, {"Div", "div", Type::I64_t()},
      {"Div", "div", Type::F64_t()}, {"Mod", "mod", Type::I32_t()},
      {"Mod", "mod", Type::I64_t()},
  };

  for (auto &e : entries) {
    std::string type_str = e.type.to_string();
    std::string key = std::string(e.class_name) + "$" + type_str;
    if (type_class_instances.contains(key))
      continue;

    TypeClassInstanceInfo info;
    info.class_name = e.class_name;
    info.concrete_type = e.type;
    std::string mangled =
        std::string(e.class_name) + "$" + type_str + "$" + e.method_name;
    info.method_mangled_names[e.method_name] = mangled;
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

// pre order — all empty, logic moved to visit() overrides
void BiTypeCheckerVisitor::preorder_walk(ProgramAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(VarDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(ExternAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(FuncDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(StructDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(EnumDefAST *ast) {}
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
void BiTypeCheckerVisitor::postorder_walk(EnumDefAST *ast) {}
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
  else if (ast->Expression) {
    ast->type = ast->Expression->accept_synthesis(this);
    // No annotation: default polymorphic literals (Integer→i32, Flt→f64)
    if (ast->type.is_polymorphic_numeric()) {
      auto concrete = default_polymorphic_type(ast->type);
      resolve_literal_type(ast->Expression.get(), concrete);
      ast->type = concrete;
    }
  } else {
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
Type BiTypeCheckerVisitor::synthesize(EnumDefAST *ast) {
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
               ast->functionName.display(), ast->type.to_string());
  });
  return ast->type;
}

Type BiTypeCheckerVisitor::synthesize(CallExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  // --- Enum variant constructor (Enum::Variant syntax) ---
  if (ast->functionName.is_qualified()) {
    auto enum_type_opt = get_typename_type(ast->functionName.module);
    if (enum_type_opt && enum_type_opt->type_kind == TypeKind::Enum) {
      auto &enum_type = *enum_type_opt;
      auto &et = std::get<EnumType>(enum_type.type_data);
      auto variant_idx = et.get_variant_index(ast->functionName.name);
      if (!variant_idx) {
        this->add_error(
            ast->get_location(),
            fmt::format("Enum '{}' has no variant '{}'",
                        ast->functionName.module, ast->functionName.name));
        return ast->type = Type::Poisoned();
      }
      auto &vi = et.get_variant(*variant_idx);

      if (ast->arguments.size() != vi.payload_types.size()) {
        this->add_error(
            ast->get_location(),
            fmt::format("Enum variant '{}::{}' expects {} arguments, got {}",
                        ast->functionName.module, vi.name,
                        vi.payload_types.size(), ast->arguments.size()));
        return ast->type = Type::Poisoned();
      }

      for (size_t i = 0; i < ast->arguments.size(); i++) {
        auto arg_type = ast->arguments[i]->accept_synthesis(this);
        if (!type_map_ordering.compatible_to_from(vi.payload_types[i],
                                                  arg_type)) {
          this->add_error(
              ast->arguments[i]->get_location(),
              fmt::format("Type mismatch in enum variant '{}::{}' argument {}: "
                          "expected {}, got {}",
                          ast->functionName.module, vi.name, i + 1,
                          vi.payload_types[i].to_string(),
                          arg_type.to_string()));
          return ast->type = Type::Poisoned();
        }
      }

      ast->is_enum_constructor = true;
      ast->enum_variant_index = *variant_idx;
      return ast->type = enum_type;
    }
  }

  // Try typeclass dispatch first (requires explicit type args)
  if (!ast->explicit_type_args.empty()) {
    auto result = synthesize_typeclass_call(ast);
    if (result.type_kind != TypeKind::NonExistent)
      return result;
  }

  // Try generic function instantiation
  if (generic_func_defs.contains(ast->functionName.mangled()))
    return synthesize_generic_call(ast);

  // Normal function call
  return synthesize_normal_call(ast);
}

Type BiTypeCheckerVisitor::synthesize_typeclass_call(CallExprAST *ast) {
  auto method_name = ast->functionName.mangled();
  auto class_it = method_to_class.find(method_name);
  if (class_it == method_to_class.end())
    return Type::NonExistent(); // Not a typeclass method — fall through

  auto &class_name = class_it->second;
  auto &tc = type_class_defs[class_name];

  Type concrete = resolve_type_expr(ast->explicit_type_args[0].get());
  if (concrete.type_kind == TypeKind::Poisoned)
    return ast->type = Type::Poisoned();

  std::string key = class_name + "$" + concrete.to_string();
  auto inst_it = type_class_instances.find(key);
  if (inst_it == type_class_instances.end()) {
    add_error(ast->get_location(),
              fmt::format("No instance of {}<{}>", class_name,
                          concrete.to_string()));
    return ast->type = Type::Poisoned();
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
    return ast->type = Type::Poisoned();
  }

  auto func_type = std::get<FunctionType>(method_proto->type.type_data);
  auto params = func_type.get_params_types();
  if (ast->arguments.size() != params.size()) {
    add_error(ast->get_location(),
              fmt::format("Type class method '{}' expects {} arguments, got {}",
                          method_name, params.size(), ast->arguments.size()));
    return ast->type = Type::Poisoned();
  }

  ast->resolved_generic_name =
      inst_it->second.method_mangled_names[method_name];
  ast->is_typeclass_call = true;

  auto return_type = func_type.get_return_type();
  std::unordered_map<std::string, Type> bindings;
  bindings[tc.type_param] = concrete;
  return ast->type = substitute(return_type, bindings);
}

Type BiTypeCheckerVisitor::synthesize_generic_call(CallExprAST *ast) {
  auto *generic_def = generic_func_defs[ast->functionName.mangled()];
  auto generic_type = generic_def->type;
  auto func = std::get<FunctionType>(generic_type.type_data);
  auto params = func.get_params_types();

  if (ast->arguments.size() != params.size()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Generic function '{}' expects {} arguments, got {}",
                    ast->functionName.display(), params.size(),
                    ast->arguments.size()));
    return ast->type = Type::Poisoned();
  }

  std::unordered_map<std::string, Type> bindings;

  if (!ast->explicit_type_args.empty()) {
    // Explicit type arguments: f<T, U>(args) — must provide all
    auto &type_params = generic_def->Prototype->type_params;
    if (ast->explicit_type_args.size() != type_params.size()) {
      this->add_error(
          ast->get_location(),
          fmt::format("Expected {} type argument(s) for '{}', got {}",
                      type_params.size(), ast->functionName.display(),
                      ast->explicit_type_args.size()));
      return ast->type = Type::Poisoned();
    }

    for (size_t i = 0; i < type_params.size(); i++) {
      Type resolved = resolve_type_expr(ast->explicit_type_args[i].get());
      if (resolved.type_kind == TypeKind::Poisoned)
        return ast->type = Type::Poisoned();
      bindings[type_params[i]] = resolved;
    }

    for (size_t i = 0; i < ast->arguments.size(); i++) {
      auto arg_type = ast->arguments[i]->accept_synthesis(this);
      if (arg_type.type_kind == TypeKind::Poisoned)
        return ast->type = Type::Poisoned();
      auto expected = substitute(params[i], bindings);
      if (!type_map_ordering.compatible_to_from(expected, arg_type)) {
        this->add_error(
            ast->arguments[i]->get_location(),
            fmt::format("Argument {} to '{}': expected {}, got {}", i + 1,
                        ast->functionName.display(), expected.to_string(),
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

  std::string mangled = ast->functionName.mangled();
  for (auto &tp : generic_def->Prototype->type_params)
    mangled += "." + bindings[tp].to_string();

  ast->resolved_generic_name = mangled;
  ast->type_bindings = bindings;
  ast->callee_func_type = generic_type;
  return ast->type = substitute(func.get_return_type(), bindings);
}

Type BiTypeCheckerVisitor::synthesize_normal_call(CallExprAST *ast) {
  auto ty = try_get_callee_type(ast->functionName.mangled());
  if (!ty.has_value()) {
    this->add_error(ast->get_location(),
                    fmt::format("Function '{}' not found",
                                ast->functionName.display()));
    return ast->type = Type::Poisoned();
  }

  ast->callee_func_type = ty;

  if (ty->type_kind != TypeKind::Function) {
    this->add_error(ast->get_location(),
                    fmt::format("'{}' is not callable",
                                ast->functionName.display()));
    return ast->type = Type::Poisoned();
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
                      ast->functionName.display(), params.size(),
                      ast->arguments.size()));
      return ast->type = Type::Poisoned();
    }
    return ast->type = func.get_return_type();
  }

  if (ast->arguments.size() > params.size()) {
    this->add_error(ast->get_location(),
                    fmt::format("Function '{}' expects {} arguments, got {}",
                                ast->functionName.display(), params.size(),
                                ast->arguments.size()));
    return ast->type = Type::Poisoned();
  }

  // Partial application: fewer args than params
  if (ast->arguments.size() < params.size()) {
    ast->is_partial = true;
    std::vector<Type> remaining;
    for (size_t i = ast->arguments.size(); i < params.size(); i++)
      remaining.push_back(params[i]);
    remaining.push_back(func.get_return_type());
    return ast->type = Type::Function(std::move(remaining));
  }

  // Arg type-checking happens in visit(CallExprAST*) after args are visited,
  // since synthesize runs before args have their types set.
  return ast->type = func.get_return_type();
}

Type BiTypeCheckerVisitor::synthesize(ReturnExprAST *ast) {
  return ast->type = Type::Never();
}
Type BiTypeCheckerVisitor::synthesize(BinaryExprAST *ast) {
  if (ast->synthesized())
    return ast->type;

  if (ast->LHS->type.type_kind == TypeKind::Never ||
      ast->RHS->type.type_kind == TypeKind::Never)
    return ast->type = Type::Never();

  // Both operands polymorphic and same kind: keep polymorphic, skip typeclass
  // But NOT for comparison/logical operators, which must return Bool
  if (ast->LHS->type.is_polymorphic_numeric() &&
      ast->RHS->type.is_polymorphic_numeric() &&
      ast->LHS->type.type_kind == ast->RHS->type.type_kind &&
      !ast->Op->is_comparison() && !ast->Op->is_logical()) {
    return ast->type = ast->LHS->type;
  }
  // One polymorphic, one concrete: resolve polymorphic to concrete
  if (ast->LHS->type.is_polymorphic_numeric() &&
      !ast->RHS->type.is_polymorphic_numeric()) {
    if (type_map_ordering.compatible_to_from(ast->RHS->type, ast->LHS->type))
      resolve_literal_type(ast->LHS.get(), ast->RHS->type);
  } else if (ast->RHS->type.is_polymorphic_numeric() &&
             !ast->LHS->type.is_polymorphic_numeric()) {
    if (type_map_ordering.compatible_to_from(ast->LHS->type, ast->RHS->type))
      resolve_literal_type(ast->RHS.get(), ast->LHS->type);
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

  return synthesize_binary_operator(ast, ast->LHS->type, ast->RHS->type);
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
    } else if (auto *idx = dynamic_cast<IndexExprAST *>(ast->LHS.get())) {
      if (auto *arr_var =
              dynamic_cast<VariableExprAST *>(idx->array_expr.get())) {
        if (!arr_var->type.is_mutable) {
          this->add_error(
              ast->Op->get_location(),
              fmt::format(
                  "Cannot write to index of immutable array '{}'. "
                  "Use 'let mut' to declare it as mutable",
                  arr_var->variableName));
        }
      }
    }
    return ast->type = Type::Unit();
  }

  if (ast->Op->is_logical())
    return ast->type = lhs_type;

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
    std::string key = class_name + "$" + lhs_type.to_string();
    auto inst_it = type_class_instances.find(key);
    if (inst_it == type_class_instances.end()) {
      add_error(ast->Op->get_location(),
                fmt::format("No instance of {}<{}> — cannot use '{}' on this "
                            "type",
                            class_name, lhs_type.to_string(),
                            ast->Op->lexeme));
      return ast->type = Type::Poisoned();
    }
    auto method_it = inst_it->second.method_mangled_names.find(method_name);
    if (method_it != inst_it->second.method_mangled_names.end())
      ast->resolved_op_method = method_it->second;
    return ast->type = lhs_type;
  }

  return ast->type = lhs_type;
}

Type BiTypeCheckerVisitor::synthesize(StringExprAST *ast) {
  if (ast->synthesized())
    return ast->type;
  // String literals are char pointers at runtime
  return ast->type = Type::Pointer(Type::Char());
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
    ast->type = Type::Integer();
  else
    ast->type = Type::Flt();

  return ast->type;
}
Type BiTypeCheckerVisitor::synthesize(BoolExprAST *ast) {
  return ast->type = Type::Bool();
}
Type BiTypeCheckerVisitor::synthesize(CharExprAST *ast) {
  return ast->type = Type::Char();
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
    if (type_map_ordering.compatible_to_from(else_type, then_type)) {
      resolve_literal_type(ast->thenBlockAST->Statements.back().get(),
                           else_type);
      ast->thenBlockAST->type = else_type;
      return ast->type = else_type;
    } else if (type_map_ordering.compatible_to_from(then_type, else_type)) {
      resolve_literal_type(ast->elseBlockAST->Statements.back().get(),
                           then_type);
      ast->elseBlockAST->type = then_type;
      return ast->type = then_type;
    }
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

  // Default polymorphic element type (annotation resolution happens in visit(VarDefAST*))
  if (first_type.is_polymorphic_numeric()) {
    auto concrete = default_polymorphic_type(first_type);
    for (auto &elem : ast->elements)
      resolve_literal_type(elem.get(), concrete);
    first_type = concrete;
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
  if (idx_type.type_kind == TypeKind::Integer) {
    resolve_literal_type(ast->index_expr.get(), Type::I32_t());
    idx_type = Type::I32_t();
  }
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
      operand_type.type_kind != TypeKind::F64_t &&
      operand_type.type_kind != TypeKind::Integer &&
      operand_type.type_kind != TypeKind::Flt) {
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
