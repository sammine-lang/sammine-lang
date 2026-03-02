#include "semantics/ScopeGeneratorVisitor.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "util/LexicalContext.h"
#include "util/Utilities.h"
//! \file ScopeGeneratorVisitor.cpp
//! \brief Implements ScopeGeneratorVisitor, an ASTVisitor that traverses the
//! AST to populate a lexical symbol table
namespace sammine_lang::AST {
// pre order
void ScopeGeneratorVisitor::preorder_walk(ProgramAST *ast) {

  // Map import-statement locations to module names so we can tell the user
  // which import introduced a conflicting name.
  std::map<std::pair<int64_t, int64_t>, std::string> import_loc_to_module;
  for (auto &imp : ast->imports)
    import_loc_to_module[{imp.location.source_start, imp.location.source_end}] =
        imp.module_name;

  std::string fn_name;
  sammine_util::Location loc;
  for (auto &def : ast->DefinitionVec) {
    if (auto fn_def = llvm::dyn_cast<FuncDefAST>(def.get())) {
      fn_name = fn_def->Prototype->functionName.mangled();
      loc = fn_def->Prototype->get_location();
    } else if (auto extern_def = llvm::dyn_cast<ExternAST>(def.get())) {
      fn_name = extern_def->Prototype->functionName.mangled();
      loc = extern_def->Prototype->get_location();
    } else if (auto record_def = llvm::dyn_cast<StructDefAST>(def.get())) {
      fn_name = record_def->struct_name.mangled();
      loc = record_def->get_location();
    } else if (auto enum_def = llvm::dyn_cast<EnumDefAST>(def.get())) {
      fn_name = enum_def->enum_name.mangled();
      loc = enum_def->get_location();
      // Register variant names for unqualified access (e.g., Red, Some, None)
      for (auto &variant : enum_def->variants) {
        if (can_see(variant.name) == nameNotFound) {
          register_name(variant.name, enum_def->get_location());
          variant_to_enum[variant.name] = enum_def->enum_name.get_name();
        }
      }
    } else if (auto alias_def = llvm::dyn_cast<TypeAliasDefAST>(def.get())) {
      fn_name = alias_def->alias_name.mangled();
      loc = alias_def->get_location();
    } else if (llvm::isa<TypeClassDeclAST>(def.get()) ||
               llvm::isa<TypeClassInstanceAST>(def.get())) {
      // Type class decls/instances don't register top-level names in scope
      continue;
    } else
      this->abort("Should not be any other def");

    if (!fn_name.empty()) {
      if (can_see(fn_name) == nameNotFound)
        register_name(fn_name, loc);
      else if (can_see(fn_name) == nameFound) {
        auto prev_loc = this->scope_stack.get_from_name(fn_name);
        auto prev_key =
            std::make_pair(prev_loc.source_start, prev_loc.source_end);
        auto imp_it = import_loc_to_module.find(prev_key);
        if (imp_it != import_loc_to_module.end()) {
          add_error(
              loc,
              fmt::format(
                  "[SCOPE]: The name `{}` conflicts with `{}` imported from '{}'",
                  fn_name, fn_name, imp_it->second));
        } else {
          add_error(loc,
                    fmt::format(
                        "[SCOPE]: The name `{}` has been introduced before",
                        fn_name));
          add_error(
              prev_loc,
              fmt::format("[SCOPE]: Most recently defined `{}` is here",
                          fn_name));
        }
      }
    }
  }
}
void ScopeGeneratorVisitor::preorder_walk(VarDefAST *ast) {
  if (ast->is_tuple_destructure) {
    for (auto &var : ast->destructure_vars) {
      auto var_name = var->name;
      if (can_see(var_name) == nameNotFound) {
        register_name(var_name, var->get_location());
      } else if (can_see(var_name) == nameFound) {
        // Allow shadowing in destructuring (needed for linear deref rebinding)
        register_name(var_name, var->get_location());
      }
    }
    return;
  }

  auto var_name = ast->TypedVar->name;
  if (can_see(var_name) == nameNotFound) {
    register_name(var_name, ast->TypedVar->get_location());
  } else if (can_see(var_name) == nameFound) {
    add_error(ast->TypedVar->get_location(),
              fmt::format("[SCOPE]: The name `{}` has been introduced before",
                          var_name));
    add_error(
        this->scope_stack.recursive_get_from_name(var_name),
        fmt::format("[SCOPE]: The firstly defined `{}` is here", var_name));
  }
}
void ScopeGeneratorVisitor::visit(ExternAST *ast) {
  scope_stack.push_context();
  ast->walk_with_preorder(this);
  ast->Prototype->accept_vis(this);
  ast->walk_with_postorder(this);
  scope_stack.pop_context();
}
void ScopeGeneratorVisitor::preorder_walk(ExternAST *ast) {
  // For imported externs (qualified function name), qualify type references
  // in the prototype so that e.g. `String` becomes `str::String`.
  if (ast->Prototype->functionName.is_qualified()) {
    insideImportedDef_ = true;
    currentImportModule_ = ast->Prototype->functionName.get_module();
    currentGenericTypeParams_ = {};
  }
}
void ScopeGeneratorVisitor::preorder_walk(FuncDefAST *ast) {
  // Detect imported generic function: name is qualified AND is_generic
  if (ast->Prototype->is_generic() &&
      ast->Prototype->functionName.is_qualified()) {
    insideImportedDef_ = true;
    currentImportModule_ = ast->Prototype->functionName.get_module();
    currentGenericTypeParams_ = ast->Prototype->type_params;
  }
}
void ScopeGeneratorVisitor::qualify_type_expr(TypeExprAST *expr) {
  if (!expr || !insideImportedDef_)
    return;

  if (auto *simple = llvm::dyn_cast<SimpleTypeExprAST>(expr)) {
    // Don't qualify generic type params (T, U, etc.)
    for (auto &tp : currentGenericTypeParams_)
      if (simple->name.get_name() == tp)
        return;
    // Don't qualify builtin types
    if (is_builtin_type_name(simple->name.get_name()))
      return;
    // Qualify if unqualified and the qualified version exists in scope
    if (!simple->name.is_qualified()) {
      auto qualified = simple->name.with_module(currentImportModule_);
      if (can_see(qualified.mangled()) == nameFound &&
          can_see(simple->name.mangled()) == nameNotFound) {
        simple->name = qualified;
      }
    }
  } else if (auto *ptr = llvm::dyn_cast<PointerTypeExprAST>(expr)) {
    qualify_type_expr(ptr->pointee.get());
  } else if (auto *arr = llvm::dyn_cast<ArrayTypeExprAST>(expr)) {
    qualify_type_expr(arr->element.get());
  } else if (auto *gen = llvm::dyn_cast<GenericTypeExprAST>(expr)) {
    if (!gen->base_name.is_qualified()) {
      auto qualified = gen->base_name.with_module(currentImportModule_);
      if (can_see(qualified.mangled()) == nameFound &&
          can_see(gen->base_name.mangled()) == nameNotFound) {
        gen->base_name = qualified;
      }
    }
    for (auto &arg : gen->type_args)
      qualify_type_expr(arg.get());
  } else if (auto *func = llvm::dyn_cast<FunctionTypeExprAST>(expr)) {
    for (auto &param : func->paramTypes)
      qualify_type_expr(param.get());
    qualify_type_expr(func->returnType.get());
  } else if (auto *tup = llvm::dyn_cast<TupleTypeExprAST>(expr)) {
    for (auto &elem : tup->element_types)
      qualify_type_expr(elem.get());
  }
}
void ScopeGeneratorVisitor::preorder_walk(StructDefAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(EnumDefAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(TypeAliasDefAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(PrototypeAST *ast) {
  // get previous scope and register the function name
  auto var_name = ast->functionName.mangled();
  if (can_see_parent(var_name) == nameNotFound)
    register_name_parent(var_name, ast->get_location());

  for (auto &param : ast->parameterVectors) {
    if (can_see(param->name) == nameFound) {
      add_error(param->get_location(),
                fmt::format("[SCOPE]: The name `{}` has been introduced before",
                            param->name));
      add_error(this->scope_stack.recursive_get_from_name(param->name),
                fmt::format("[SCOPE]: The firstly defined `{}` is here",
                            param->name));
    } else {
      register_name(param->name, param->get_location());
    }
  }

  // Qualify return type expression for imported generic functions
  if (insideImportedDef_ && ast->return_type_expr) {
    qualify_type_expr(ast->return_type_expr.get());
  }
}
void ScopeGeneratorVisitor::preorder_walk(CallExprAST *ast) {
  if (insideImportedDef_) {
    for (auto &type_arg : ast->explicit_type_args) {
      qualify_type_expr(type_arg.get());
    }
  }
}
void ScopeGeneratorVisitor::preorder_walk(ReturnExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(BinaryExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(NumberExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(StringExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(BoolExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(CharExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(VariableExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(BlockAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(IfExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(UnitExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(TypedVarAST *ast) {
  if (insideImportedDef_ && ast->type_expr) {
    qualify_type_expr(ast->type_expr.get());
  }
}
void ScopeGeneratorVisitor::preorder_walk(DerefExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(AddrOfExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(AllocExprAST *ast) {
  if (insideImportedDef_ && ast->type_arg) {
    qualify_type_expr(ast->type_arg.get());
  }
}
void ScopeGeneratorVisitor::preorder_walk(FreeExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(ArrayLiteralExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(IndexExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(LenExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(UnaryNegExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(StructLiteralExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(FieldAccessExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(CaseExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(WhileExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(TupleLiteralExprAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(TypeClassDeclAST *ast) {}
void ScopeGeneratorVisitor::preorder_walk(TypeClassInstanceAST *ast) {}

// post order
void ScopeGeneratorVisitor::postorder_walk(ProgramAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(VarDefAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(ExternAST *ast) {
  if (ast->Prototype->functionName.is_qualified()) {
    insideImportedDef_ = false;
    currentImportModule_.clear();
  }
}
void ScopeGeneratorVisitor::postorder_walk(FuncDefAST *ast) {
  insideImportedDef_ = false;
  currentImportModule_.clear();
  currentGenericTypeParams_.clear();
}
void ScopeGeneratorVisitor::postorder_walk(StructDefAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(EnumDefAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(TypeAliasDefAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(PrototypeAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(CallExprAST *ast) {
  // Qualify call names with explicit type args in imported generic bodies
  // (must run BEFORE the early return below)
  if (!ast->explicit_type_args.empty() && insideImportedDef_ &&
      !ast->functionName.is_qualified()) {
    auto qualified = ast->functionName.with_module(currentImportModule_);
    if (can_see(qualified.mangled()) == nameFound) {
      ast->functionName = qualified;
    }
  }
  // Type class calls (e.g. sizeof<i32>()) have explicit type args and are
  // resolved by the type checker, not by scope lookup.
  if (!ast->explicit_type_args.empty())
    return;

  if (ast->functionName.is_unresolved()) {
    // Allow qualified names where the prefix is an enum name in scope
    // (e.g. Color::Red) — variant resolution deferred to type checker
    if (can_see(ast->functionName.get_module()) == nameFound)
      return;
    add_error(ast->get_location(),
              fmt::format("Module '{}' is not imported",
                          ast->functionName.get_module()));
    return;
  }

  // Rewrite unqualified enum variant names to qualified form
  if (!ast->functionName.is_qualified()) {
    auto it = variant_to_enum.find(ast->functionName.get_name());
    if (it != variant_to_enum.end()) {
      if (insideImportedDef_) {
        // math::Color::Red (3-part qualified name)
        ast->functionName = sammine_util::QualifiedName::from_parts(
            {currentImportModule_, it->second, ast->functionName.get_name()});
      } else {
        // Color::Red (existing behavior)
        ast->functionName = sammine_util::QualifiedName::qualified(
            it->second, ast->functionName.get_name());
      }
      return;
    }
  }

  auto var_name = ast->functionName.mangled();
  if (can_see(var_name) == nameNotFound) {
    // Try qualifying with import module prefix for imported generic bodies
    if (insideImportedDef_ && !ast->functionName.is_qualified()) {
      auto qualified = ast->functionName.with_module(currentImportModule_);
      if (can_see(qualified.mangled()) == nameFound) {
        ast->functionName = qualified;
        return;
      }
    }
    if (ast->functionName.is_qualified() &&
        can_see(ast->functionName.get_qualifier()) == nameFound)
      return;
    add_error(
        ast->get_location(),
        fmt::format(
            "The called name {} for the call expression is not found before",
            ast->functionName.mangled()));
  }
}

void ScopeGeneratorVisitor::postorder_walk(ReturnExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(BinaryExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(NumberExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(StringExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(BoolExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(CharExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(VariableExprAST *ast) {

  auto var_name = ast->variableName;
  if (can_see(var_name) == nameNotFound) {
    // Try qualifying with import module prefix for imported generic bodies
    if (insideImportedDef_) {
      auto qualified =
          sammine_util::QualifiedName::qualified(currentImportModule_, var_name);
      if (can_see(qualified.mangled()) == nameFound) {
        ast->variableName = qualified.mangled();
        return;
      }
    }
    add_error(ast->get_location(),
              fmt::format("The variable named {} for the variable expression "
                          "is not found before",
                          var_name));
  }
}
void ScopeGeneratorVisitor::postorder_walk(BlockAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(IfExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(UnitExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(TypedVarAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(DerefExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(AddrOfExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(AllocExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(FreeExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(ArrayLiteralExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(IndexExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(LenExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(UnaryNegExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(StructLiteralExprAST *ast) {
  // Qualify struct names in imported generic function bodies
  if (insideImportedDef_ && !ast->struct_name.is_qualified()) {
    auto qualified = ast->struct_name.with_module(currentImportModule_);
    if (can_see(qualified.mangled()) == nameFound &&
        can_see(ast->struct_name.mangled()) == nameNotFound) {
      ast->struct_name = qualified;
    }
  }
}
void ScopeGeneratorVisitor::postorder_walk(FieldAccessExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(CaseExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(WhileExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(TupleLiteralExprAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(TypeClassDeclAST *ast) {}
void ScopeGeneratorVisitor::postorder_walk(TypeClassInstanceAST *ast) {}
} // namespace sammine_lang::AST
