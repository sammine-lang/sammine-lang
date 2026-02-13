#include "typecheck/BiTypeChecker.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "typecheck/Types.h"
#include "util/LexicalContext.h"
#include "util/Utilities.h"
#include <ranges>

//! \file BiTypeChecker.cpp
//! \brief Implementation of BiTypeCheckerVisitor, an ASTVisitor that
//!        traverses the AST to synthesize node types, perform bidirectional
//!        consistency checks, and register functions and variables.
namespace sammine_lang::AST {
// visit overrides — explicit traversal order

void BiTypeCheckerVisitor::visit(ProgramAST *ast) {
  for (auto &def : ast->DefinitionVec)
    def->accept_vis(this);
}

void BiTypeCheckerVisitor::visit(FuncDefAST *ast) {
  enter_new_scope();
  id_to_type.top().setScope(ast);
  typename_to_type.top().setScope(ast);
  ast->Prototype->accept_vis(this);
  ast->accept_synthesis(this);
  ast->Block->accept_vis(this);
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
  ast->Expression->accept_vis(this);
  ast->accept_synthesis(this);
  auto to = ast->type;
  auto from = ast->Expression->accept_synthesis(this);
  if (!type_map_ordering.compatible_to_from(to, from)) {
    this->add_error(
        ast->get_location(),
        fmt::format("Type mismatch in variable definition: Synthesized {}, "
                    "checked against {}.",
                    to.to_string(), from.to_string()));
    ast->type = Type::Poisoned();
  }
}

void BiTypeCheckerVisitor::visit(ExternAST *ast) {
  ast->Prototype->accept_vis(this);
}

void BiTypeCheckerVisitor::visit(RecordDefAST *ast) {}

void BiTypeCheckerVisitor::visit(CallExprAST *ast) {
  ast->accept_synthesis(this);
  for (auto &arg : ast->arguments)
    arg->accept_vis(this);
  if (!pre_func.contains(ast->functionName)) {
    auto ty = get_type_from_id_parent(ast->functionName);
    auto func = std::get<FunctionType>(ty->type_data);
    auto params = func.get_params_types();
    if (ast->arguments.size() != params.size()) {
      this->add_error(
          ast->get_location(),
          fmt::format(
              "Function '{}' params and arguments have a type mismatch",
              ast->functionName));
    }
    for (const auto &[arg, par] :
         std::ranges::views::zip(ast->arguments, params)) {
      if (!this->type_map_ordering.compatible_to_from(par, arg->type)) {
        this->add_error(
            ast->get_location(),
            fmt::format(
                "Function '{}' params and arguments have a type mismatch",
                ast->functionName));
      }
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

// pre order — all empty, logic moved to visit() overrides
void BiTypeCheckerVisitor::preorder_walk(ProgramAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(VarDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(ExternAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(FuncDefAST *ast) {}
void BiTypeCheckerVisitor::preorder_walk(RecordDefAST *ast) {}
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

// post order — all empty, logic moved to visit() overrides
void BiTypeCheckerVisitor::postorder_walk(ProgramAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(VarDefAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(ExternAST *ast) {}
void BiTypeCheckerVisitor::postorder_walk(RecordDefAST *ast) {}
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

  id_to_type.registerNameT(ast->TypedVar->name, ast->type);
  return ast->type;
}

Type BiTypeCheckerVisitor::synthesize(ExternAST *ast) {
  return Type::NonExistent();
}
Type BiTypeCheckerVisitor::synthesize(RecordDefAST *ast) {
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
  ast->type = Type::Function(std::move(v));

  return ast->type;
}

Type BiTypeCheckerVisitor::synthesize(CallExprAST *ast) {
  if (ast->synthesized() || pre_func.contains(ast->functionName))
    return ast->type;
  auto ty = get_type_from_id_parent(ast->functionName);
  switch (ty->type_kind) {
  case TypeKind::Function:
    return ast->type = std::get<FunctionType>(ty->type_data).get_return_type();
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
  case TypeKind::Record:
  case TypeKind::Poisoned:
  case TypeKind::Integer:
  case TypeKind::Flt:
    this->abort(fmt::format("should not happen here with function {}",
                            ast->functionName));
    break;
  }
  return Type::NonExistent();
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
                                                  ast->RHS->type))
    this->abort();
  if (ast->Op->is_comparison())
    return ast->type = Type::Bool();
  if (ast->Op->is_assign()) 
    return ast->type = Type::Unit();

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
      this->abort_on(true,
                     fmt::format("invalid type suffix '{}' on number literal",
                                 suffix));
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
  return ast->type = id_to_type.get_from_name(ast->variableName);
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
    this->add_error(ast->get_location(),
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
        ast->get_location(),
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
  return ast->type = std::get<PointerType>(operand_type.type_data).get_pointee();
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

  auto operand_type = ast->operand->accept_synthesis(this);
  return ast->type = Type::Pointer(operand_type);
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
    this->add_error(ast->get_location(), "Array literal must have at least one element");
    return ast->type = Type::Poisoned();
  }

  auto first_type = ast->elements[0]->accept_synthesis(this);
  for (size_t i = 1; i < ast->elements.size(); i++) {
    auto elem_type = ast->elements[i]->accept_synthesis(this);
    if (elem_type != first_type) {
      this->add_error(ast->get_location(),
                      fmt::format("Array literal element type mismatch: expected {}, got {}",
                                  first_type.to_string(), elem_type.to_string()));
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
    this->add_error(ast->get_location(),
                    fmt::format("Cannot index non-array type '{}'",
                                arr_type.to_string()));
    return ast->type = Type::Poisoned();
  }

  auto idx_type = ast->index_expr->accept_synthesis(this);
  if (idx_type.type_kind != TypeKind::I32_t && idx_type.type_kind != TypeKind::I64_t) {
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
      this->add_error(ast->get_location(),
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

} // namespace sammine_lang::AST
