//
// Created by Jasmine Tang on 3/27/24.
//

#include "codegen/CodegenVisitor.h"
#include "ast/Ast.h"
#include "codegen/CodegenUtils.h"
#include "lex/Token.h"
#include "util/Logging.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "codegen"

//! \file CodegenVisitor.cpp
//! \brief Implementation for CodegenVisitor, it converts the AST Representation
//! into LLVM IR and it also uses a visitor pattern in order to traverse through
//! the parsed AST to emit LLVM IR.
namespace sammine_lang::AST {
using llvm::BasicBlock;

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
///
/// from the llvm blog:
/// mem2reg only looks for alloca instructions in the entry block of the
/// function. Being in the entry block guarantees that the alloca is only
/// executed once, which makes analysis simpler.

llvm::Function *CgVisitor::getCurrentFunction() { return this->current_func; }
void CgVisitor::enter_new_scope() {
  allocaValues.push(std::map<std::string, llvm::AllocaInst *>());
}
void CgVisitor::exit_new_scope() { allocaValues.pop(); }

void CgVisitor::setCurrentFunction(llvm::Function *func) {

  this->current_func = func;
}
void CgVisitor::visit(FuncDefAST *ast) {

  this->enter_new_scope();
  ast->Prototype->accept_vis(this);
  ast->walk_with_preorder(this);
  ast->Block->accept_vis(this);
  ast->walk_with_postorder(this);
  this->exit_new_scope();
}

void CgVisitor::preorder_walk(ProgramAST *ast) {
  // TODO: In the future, we need to move both this function someplace else.
  //
  // INFO: To use for both function decl, malloc and printf
  llvm::PointerType *int8ptr = llvm::PointerType::get(
      *this->resPtr->Context, 0); // 0 stands for generic address space
                                  //
  CodegenUtils::declare_fn(*this->resPtr->Module, "printf",
                           llvm::Type::getInt32Ty(*this->resPtr->Context),
                           int8ptr, true);
}

void CgVisitor::preorder_walk(VarDefAST *ast) {
  auto var_name = ast->TypedVar->name;
  LOG({
    fmt::print("[CODEGEN] Codegen preorder_walk for VarDefAST for {}\n",
               ast->TypedVar->name);
  });
  auto alloca = CodegenUtils::CreateEntryBlockAlloca(
      getCurrentFunction(), var_name, type_converter.get_type(ast->type));
  this->allocaValues.top()[var_name] = alloca;
}
void CgVisitor::postorder_walk(VarDefAST *ast) {
  auto var_name = ast->TypedVar->name;
  auto alloca = this->allocaValues.top()[var_name];

  LOG({
    fmt::print("[CODEGEN] Codegen postorder_walk for VarDefAST for {}\n",
               ast->TypedVar->name);
  });
  if (ast->Expression == nullptr) {
    this->abort_if_not(ast->Expression, "is this legal?");
  } else {
    resPtr->Builder->CreateStore(ast->Expression->val, alloca);
  }
}
void CgVisitor::preorder_walk(ExternAST *ast) {}
void CgVisitor::preorder_walk(RecordDefAST *ast) {}
void CgVisitor::postorder_walk(RecordDefAST *ast) {}

void CgVisitor::postorder_walk(ReturnExprAST *ast) {
  // INFO: If we cannot parse return expr, treat it as unit for now
  if (!ast->return_expr || ast->return_expr->type == Type::Unit())
    resPtr->Builder->CreateRetVoid();
  else
    resPtr->Builder->CreateRet(ast->return_expr->val);
}

void CgVisitor::preorder_walk(BinaryExprAST *ast) {}

void CgVisitor::postorder_walk(BinaryExprAST *ast) {
  if (ast->Op->tok_type == TokenType::TokASSIGN) {
    VariableExprAST *LHSE = static_cast<VariableExprAST *>(ast->LHS.get());
    if (!LHSE) {
      this->abort("Left hand side of assignment must be a variable, "
                  "current LHS cannot "
                  "be statically cast to VarExprAST");
      return;
    }

    auto R = ast->RHS->val;

    if (!R)
      this->abort("Failed to codegen RHS for tok assign");

    auto *Var = this->allocaValues.top()[LHSE->variableName];
    if (!Var)
      this->abort("Unknown variable in LHS of tok assign");

    resPtr->Builder->CreateStore(R, Var);
    ast->val = R;

    return;
  }
  auto L = ast->LHS->val;
  auto R = ast->RHS->val;

  if (ast->Op->tok_type == TokenType::TokADD) {
    if (ast->LHS->type == Type::I64_t())
      ast->val = resPtr->Builder->CreateAdd(L, R, "add_expr");
    else if (ast->LHS->type == Type::F64_t())
      ast->val = resPtr->Builder->CreateFAdd(L, R, "add_expr");
    else
      this->abort();
  }
  if (ast->Op->tok_type == TokenType::TokSUB) {
    if (ast->LHS->type == Type::I64_t())
      ast->val = resPtr->Builder->CreateSub(L, R, "sub_expr");
    else if (ast->LHS->type == Type::F64_t())
      ast->val = resPtr->Builder->CreateFSub(L, R, "sub_expr");
    else
      this->abort();
  }
  if (ast->Op->tok_type == TokenType::TokMUL) {
    if (ast->LHS->type == Type::I64_t())
      ast->val = resPtr->Builder->CreateMul(L, R, "mul_expr");
    else if (ast->LHS->type == Type::F64_t())
      ast->val = resPtr->Builder->CreateFMul(L, R, "mul_expr");
    else
      this->abort();
  }
  if (ast->Op->tok_type == TokenType::TokDIV) {
    if (ast->LHS->type == Type::F64_t())
      ast->val = resPtr->Builder->CreateFDiv(L, R, "div_expr");
    else
      this->abort();
  }
  if (ast->Op->tok_type == TokOR) {
    ast->val = resPtr->Builder->CreateLogicalOr(ast->LHS->val, ast->RHS->val);
  }
  if (ast->Op->tok_type == TokAND) {
    ast->val = resPtr->Builder->CreateLogicalAnd(ast->LHS->val, ast->RHS->val);
  }
  if (ast->Op->is_comparison()) {
    /*auto cmp_int = resPtr->Builder->CreateFCmpULT(L, R, "less_cmp_expr");*/
    ast->val = resPtr->Builder->CreateCmp(
        type_converter.get_cmp_func(ast->LHS->type, ast->RHS->type,
                                    ast->Op->tok_type),
        L, R);
  }
  if (ast->Op->tok_type == TokMOD) {
    ast->val = resPtr->Builder->CreateSRem(L, R);
  }
  if (!ast->val) {
    std::cout << ast->Op->lexeme << std::endl;
    this->abort();
  }

  return;
}

void CgVisitor::preorder_walk(StringExprAST *ast) {}
void CgVisitor::preorder_walk(NumberExprAST *ast) {
  switch (ast->type.type_kind) {
  case TypeKind::I64_t:
    ast->val = llvm::ConstantInt::get(
        *resPtr->Context, llvm::APInt(64, std::stoi(ast->number), true));
    break;
  case TypeKind::F64_t:
    ast->val = llvm::ConstantFP::get(*resPtr->Context,
                                     llvm::APFloat(std::stod(ast->number)));
    break;
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::Function:
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::String:
  case TypeKind::Record:
    this->abort(".....");
  }
  this->abort_if_not(ast->val, "cannot generate number");
}
void CgVisitor::preorder_walk(BoolExprAST *ast) {
  if (ast->b)
    ast->val = llvm::ConstantFP::get(*resPtr->Context,
                                     llvm::APFloat(std::stod("1.0")));
  else
    ast->val = llvm::ConstantFP::get(*resPtr->Context,
                                     llvm::APFloat(std::stod("0.0")));
}
void CgVisitor::preorder_walk(VariableExprAST *ast) {
  auto *alloca = this->allocaValues.top()[ast->variableName];

  this->abort_if_not(alloca, "Unknown variable name");

  ast->val = resPtr->Builder->CreateLoad(alloca->getAllocatedType(), alloca,
                                         ast->variableName);
}
void CgVisitor::preorder_walk(UnitExprAST *ast) {}
void CgVisitor::preorder_walk(IfExprAST *ast) {}

// Override visit to control child traversal order for if-expr codegen
void CgVisitor::visit(IfExprAST *ast) {
  // First, codegen the condition
  ast->bool_expr->accept_vis(this);
  if (!ast->bool_expr->val) {
    this->abort("Failed to codegen condition of if-expr");
  }

  // Convert condition to i1 based on type
  switch (ast->bool_expr->type.type_kind) {
  case TypeKind::I64_t:
    ast->bool_expr->val = resPtr->Builder->CreateFCmpONE(
        resPtr->Builder->CreateSIToFP(
            ast->bool_expr->val, llvm::Type::getDoubleTy(*resPtr->Context)),
        llvm::ConstantFP::get(*resPtr->Context, llvm::APFloat(0.0)),
        "ifcond_i64");
    break;
  case TypeKind::F64_t:
    ast->bool_expr->val = resPtr->Builder->CreateFCmpONE(
        ast->bool_expr->val,
        llvm::ConstantFP::get(*resPtr->Context, llvm::APFloat(0.0)),
        "ifcond_f64");
    break;
  case TypeKind::NonExistent:
    ASTPrinter::print(ast->bool_expr.get());
    this->abort("Invalid syntax for now");
    break;
  case TypeKind::Poisoned:
    this->abort("Invalid syntax for now");
    break;
  case TypeKind::Unit:
    this->abort("Invalid syntax for now");
    break;
  case TypeKind::Record:
    this->abort("Invalid syntax for now");
    break;
  case TypeKind::Bool:
    ast->bool_expr->val = resPtr->Builder->CreateFCmpONE(
        resPtr->Builder->CreateSIToFP(
            ast->bool_expr->val, llvm::Type::getDoubleTy(*resPtr->Context)),
        llvm::ConstantFP::get(*resPtr->Context, llvm::APFloat(0.0)),
        "ifcond_bool");
    break;
  case TypeKind::Function:
    this->abort(
        "Invalid syntax for now, typechecker should caught this function");
    break;
  case TypeKind::Never:
    this->abort("Never type should not reach codegen for if condition");
    break;
  case TypeKind::String:
    this->abort("Cannot turn str to boolean, typecheck should have "
                "caught this string");
    break;
  }

  llvm::Function *function = resPtr->Builder->GetInsertBlock()->getParent();

  // Create blocks for the then and else cases
  BasicBlock *ThenBB = BasicBlock::Create(*resPtr->Context, "then", function);
  BasicBlock *ElseBB = BasicBlock::Create(*resPtr->Context, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*resPtr->Context, "ifcont");

  resPtr->Builder->CreateCondBr(ast->bool_expr->val, ThenBB, ElseBB);

  // Codegen then block
  resPtr->Builder->SetInsertPoint(ThenBB);
  ast->thenBlockAST->accept_vis(this);

  // Get the value from the then block (last statement's value)
  llvm::Value *ThenV = nullptr;
  if (!ast->thenBlockAST->Statements.empty()) {
    ThenV = ast->thenBlockAST->Statements.back()->val;
  }
  // Get the actual block we're in now (codegen might have created new blocks)
  BasicBlock *ThenEndBB = resPtr->Builder->GetInsertBlock();
  if (!ThenEndBB->getTerminator())
    resPtr->Builder->CreateBr(MergeBB);

  // Codegen else block
  function->insert(function->end(), ElseBB);
  resPtr->Builder->SetInsertPoint(ElseBB);
  ast->elseBlockAST->accept_vis(this);

  // Get the value from the else block (last statement's value)
  llvm::Value *ElseV = nullptr;
  if (!ast->elseBlockAST->Statements.empty()) {
    ElseV = ast->elseBlockAST->Statements.back()->val;
  }
  // Get the actual block we're in now
  BasicBlock *ElseEndBB = resPtr->Builder->GetInsertBlock();
  if (!ElseEndBB->getTerminator())
    resPtr->Builder->CreateBr(MergeBB);

  // Continue at merge block only if it has predecessors
  if (MergeBB->hasNPredecessorsOrMore(1)) {
    function->insert(function->end(), MergeBB);
    resPtr->Builder->SetInsertPoint(MergeBB);

    // Create PHI node to merge values if both branches produce values
    // and the if expression has a non-Unit, non-Never type
    if (ThenV && ElseV && ast->type.type_kind != TypeKind::Unit &&
        ast->type.type_kind != TypeKind::Never) {
      llvm::PHINode *PN =
          resPtr->Builder->CreatePHI(ThenV->getType(), 2, "iftmp");
      PN->addIncoming(ThenV, ThenEndBB);
      PN->addIncoming(ElseV, ElseEndBB);
      ast->val = PN;
    }
  } else {
    // Both branches terminated, no merge block needed
    delete MergeBB;
  }
}
void CgVisitor::preorder_walk(TypedVarAST *ast) {}

void CgVisitor::postorder_walk(StringExprAST *ast) {
  // Allocate memory for string using ref-counted malloc wrapper
  std::string stringContent = ast->string_content;
  ast->val = this->resPtr->Builder->CreateGlobalString(stringContent);
  ast->type = Type::String(stringContent);
}

} // namespace sammine_lang::AST
