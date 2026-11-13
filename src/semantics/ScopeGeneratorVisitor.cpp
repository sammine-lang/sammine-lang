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
  // Top-level names are each module's, kept by the resolver; the scopes
  // hold locals.
  resolver.use_program(*ast);
  for (auto &def : ast->DefinitionVec)
    if (auto *enum_def = llvm::dyn_cast<EnumDefAST>(def.get()))
      for (auto &variant : enum_def->variants)
        variant_to_enum[variant.name].push_back(enum_def->enum_name);
}
void ScopeGeneratorVisitor::preorder_walk(VarDefAST *ast) {
  // A let may shadow: `let x = x + 1`, and `let (a, b) = ...` too.
  if (ast->is_tuple_destructure) {
    for (auto &var : ast->destructure_vars)
      register_name(var->name, var->get_location());
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
  // A parameter may not take a name its definition's code already has:
  // another parameter, or a top-level name in its module's scope. An
  // importer's names aren't, though its code may be compiled there.
  for (auto &param : ast->parameterVectors) {
    if (auto taken = resolver.defined_at(param->name, ctx())) {
      add_error(param->get_location(),
                fmt::format("[SCOPE]: The name `{}` has been introduced before",
                            param->name));
      add_error(*taken, fmt::format("[SCOPE]: The firstly defined `{}` is here",
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
