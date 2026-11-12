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
  resolver.use_program(*ast);

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
        }
        variant_to_enum[variant.name].push_back(enum_def->enum_name);
      }
    } else if (auto alias_def = llvm::dyn_cast<TypeAliasDefAST>(def.get())) {
      fn_name = alias_def->alias_name.mangled();
      loc = alias_def->get_location();
    } else if (llvm::isa<TypeClassDeclAST>(def.get()) ||
               llvm::isa<TypeClassInstanceAST>(def.get())) {
      // Type class decls/instances don't register top-level names in scope
      continue;
    } else if (auto kd = llvm::dyn_cast<KernelDefAST>(def.get())) {
      fn_name = kd->Prototype->functionName.mangled();
      loc = kd->Prototype->get_location();
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
          add_error(loc, fmt::format("[SCOPE]: The name `{}` conflicts with "
                                     "`{}` imported from '{}'",
                                     fn_name, fn_name, imp_it->second));
        } else {
          add_error(
              loc,
              fmt::format("[SCOPE]: The name `{}` has been introduced before",
                          fn_name));
          add_error(prev_loc,
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
  register_name(var_name, ast->TypedVar->get_location());
}
void ScopeGeneratorVisitor::visit(ExternAST *ast) {
  enter_new_scope();
  ast->walk_with_preorder(this);
  ast->Prototype->accept_vis(this);
  ast->walk_with_postorder(this);
  exit_new_scope();
}
void ScopeGeneratorVisitor::enter_definition(
    const DefinitionAST &def, const std::vector<std::string> &type_params) {
  ctx().module = def.defining_module;
  ctx().type_params = type_params;
}

// Where names are written: each is resolved by `resolver`.

void ScopeGeneratorVisitor::preorder_walk(ExternAST *ast) {
  enter_definition(*ast, ast->Prototype->type_params);
}
void ScopeGeneratorVisitor::preorder_walk(FuncDefAST *ast) {
  enter_definition(*ast, ast->Prototype->type_params);
}
void ScopeGeneratorVisitor::visit(CaseExprAST *ast) {
  ast->scrutinee->accept_vis(this);
  for (auto &arm : ast->arms) {
    resolver.resolve_variant(arm.pattern.variant_name, arm.pattern.location,
                             ctx());
    enter_new_scope();
    for (auto &binding : arm.pattern.bindings)
      register_name(binding, arm.pattern.location);
    arm.body->accept_vis(this);
    exit_new_scope();
  }
}
// An instance is code of its module like a function. Its methods' names
// aren't registered: they are reached through the typeclass.
void ScopeGeneratorVisitor::visit(TypeClassInstanceAST *ast) {
  push_ast_context();
  enter_definition(*ast, ast->type_params);
  for (auto &type : ast->concrete_type_exprs)
    resolve_type(type.get());
  pop_ast_context();
  for (auto &method : ast->methods) {
    ScopeGuard scope(*this);
    enter_definition(*ast, ast->type_params);
    for (auto &param : method->Prototype->parameterVectors) {
      register_name(param->name, param->get_location());
      resolve_type(param->type_expr.get());
    }
    resolve_type(method->Prototype->return_type_expr.get());
    method->Block->accept_vis(this);
  }
}
void ScopeGeneratorVisitor::preorder_walk(StructDefAST *ast) {
  push_ast_context();
  enter_definition(*ast, ast->type_params);
  for (auto &member : ast->struct_members)
    resolve_type(member->type_expr.get());
  pop_ast_context();
}
void ScopeGeneratorVisitor::preorder_walk(EnumDefAST *ast) {
  push_ast_context();
  enter_definition(*ast, ast->type_params);
  for (auto &variant : ast->variants)
    for (auto &payload : variant.payload_types)
      resolve_type(payload.get());
  pop_ast_context();
}
void ScopeGeneratorVisitor::preorder_walk(TypeAliasDefAST *ast) {
  push_ast_context();
  enter_definition(*ast, {});
  resolve_type(ast->type_expr.get());
  pop_ast_context();
}
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
  resolve_type(ast->return_type_expr.get());
}
void ScopeGeneratorVisitor::preorder_walk(TypedVarAST *ast) {
  resolve_type(ast->type_expr.get());
}
void ScopeGeneratorVisitor::preorder_walk(AllocExprAST *ast) {
  resolve_type(ast->type_arg.get());
}
void ScopeGeneratorVisitor::preorder_walk(CastExprAST *ast) {
  resolve_type(ast->target_type.get());
}
void ScopeGeneratorVisitor::preorder_walk(CallExprAST *ast) {
  for (auto &type_arg : ast->explicit_type_args)
    resolve_type(type_arg.get());
}
void ScopeGeneratorVisitor::preorder_walk(VariableExprAST *ast) {
  for (auto &type_arg : ast->explicit_type_args)
    resolve_type(type_arg.get());
}
void ScopeGeneratorVisitor::postorder_walk(CallExprAST *ast) {
  resolver.resolve_value(ast->functionName, !ast->explicit_type_args.empty(),
                         /*is_call=*/true, ast->get_location(), ctx());
}
void ScopeGeneratorVisitor::postorder_walk(VariableExprAST *ast) {
  resolver.resolve_value(ast->variableName, !ast->explicit_type_args.empty(),
                         /*is_call=*/false, ast->get_location(), ctx());
}
void ScopeGeneratorVisitor::postorder_walk(StructLiteralExprAST *ast) {
  resolver.resolve_type_name(ast->struct_name, ast->get_location(), ctx());
  for (auto &type_arg : ast->explicit_type_args)
    resolve_type(type_arg.get());
}
} // namespace sammine_lang::AST
