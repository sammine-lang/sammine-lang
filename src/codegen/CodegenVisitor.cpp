//
// Created by Jasmine Tang on 3/27/24.
//

#include "codegen/CodegenVisitor.h"
#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "codegen/CodegenUtils.h"
#include "lex/Token.h"
#include "util/Logging.h"
#include "util/Utilities.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <llvm/IR/BasicBlock.h>
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

  CodegenUtils::declare_malloc(*this->resPtr->Module);
  CodegenUtils::declare_free(*this->resPtr->Module);
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
void CgVisitor::preorder_walk(RecordDefAST *ast) {
  this->abort("You forgot to implement record");
}
void CgVisitor::postorder_walk(RecordDefAST *ast) {}

void CgVisitor::postorder_walk(ReturnExprAST *ast) {
  // INFO: If we cannot parse return expr, treat it as unit for now
  if (ast->type == Type::Unit())
    resPtr->Builder->CreateRetVoid();
  else
    resPtr->Builder->CreateRet(ast->return_expr->val);
}

void CgVisitor::preorder_walk(BinaryExprAST *ast) {}

void CgVisitor::postorder_walk(BinaryExprAST *ast) {
  if (ast->Op->tok_type == TokenType::TokASSIGN) {
    auto R = ast->RHS->val;
    if (!R)
      this->abort("Failed to codegen RHS for tok assign");

    if (auto *LHSE = dynamic_cast<VariableExprAST *>(ast->LHS.get())) {
      auto *Var = this->allocaValues.top()[LHSE->variableName];
      if (!Var)
        this->abort("Unknown variable in LHS of tok assign");
      resPtr->Builder->CreateStore(R, Var);
      ast->val = R;
      return;
    }

    if (auto *deref = dynamic_cast<DerefExprAST *>(ast->LHS.get())) {
      auto *ptr = deref->operand->val;
      if (!ptr)
        this->abort("Failed to codegen pointer for deref assignment");
      resPtr->Builder->CreateStore(R, ptr);
      ast->val = R;
      return;
    }

    this->abort("Left hand side of assignment must be a variable or "
                "dereferenced pointer");
    return;
  }
  auto L = ast->LHS->val;
  auto R = ast->RHS->val;

  if (ast->Op->tok_type == TokenType::TokADD) {
    if (ast->LHS->type == Type::I32_t() || ast->LHS->type == Type::I64_t())
      ast->val = resPtr->Builder->CreateAdd(L, R, "add_expr");
    else if (ast->LHS->type == Type::F64_t())
      ast->val = resPtr->Builder->CreateFAdd(L, R, "add_expr");
    else
      this->abort();
  }
  if (ast->Op->tok_type == TokenType::TokSUB) {
    if (ast->LHS->type == Type::I32_t() || ast->LHS->type == Type::I64_t())
      ast->val = resPtr->Builder->CreateSub(L, R, "sub_expr");
    else if (ast->LHS->type == Type::F64_t())
      ast->val = resPtr->Builder->CreateFSub(L, R, "sub_expr");
    else
      this->abort();
  }
  if (ast->Op->tok_type == TokenType::TokMUL) {
    if (ast->LHS->type == Type::I32_t() || ast->LHS->type == Type::I64_t())
      ast->val = resPtr->Builder->CreateMul(L, R, "mul_expr");
    else if (ast->LHS->type == Type::F64_t())
      ast->val = resPtr->Builder->CreateFMul(L, R, "mul_expr");
    else
      this->abort();
  }
  if (ast->Op->tok_type == TokenType::TokDIV) {
    if (ast->LHS->type == Type::I32_t() || ast->LHS->type == Type::I64_t())
      ast->val = resPtr->Builder->CreateSDiv(L, R, "div_expr");
    else if (ast->LHS->type == Type::F64_t())
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
    LOG({ std::cout << ast->Op->lexeme << std::endl; });
    this->abort();
  }

  return;
}

void CgVisitor::preorder_walk(StringExprAST *ast) {}
void CgVisitor::preorder_walk(NumberExprAST *ast) {
  switch (ast->type.type_kind) {
  case TypeKind::I32_t:
    ast->val = llvm::ConstantInt::get(
        *resPtr->Context, llvm::APInt(32, std::stoi(ast->number), true));
    break;
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
  case TypeKind::Pointer:
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::String:
  case TypeKind::Record:
    this->abort("NumberExprAST has invalid type kind");
  }
  this->abort_if_not(ast->val, "cannot generate number");
}
void CgVisitor::preorder_walk(BoolExprAST *ast) {
  ast->val = llvm::ConstantFP::get(
      *resPtr->Context, llvm::APFloat(std::stod(ast->b ? "1.0" : "0.0")));
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
  case TypeKind::I32_t:
    ast->bool_expr->val = resPtr->Builder->CreateICmpNE(
        ast->bool_expr->val,
        llvm::ConstantInt::get(*resPtr->Context, llvm::APInt(32, 0)),
        "ifcond_i32");
    break;
  case TypeKind::I64_t:
    ast->bool_expr->val = resPtr->Builder->CreateICmpNE(
        ast->bool_expr->val,
        llvm::ConstantInt::get(*resPtr->Context, llvm::APInt(64, 0)),
        "ifcond_i64");
    break;
  case TypeKind::F64_t:
    ast->bool_expr->val = resPtr->Builder->CreateFCmpONE(
        ast->bool_expr->val,
        llvm::ConstantFP::get(*resPtr->Context, llvm::APFloat(0.0)),
        "ifcond_f64");
    break;
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::Unit:
  case TypeKind::Record:
  case TypeKind::Function:
  case TypeKind::Pointer:
  case TypeKind::Never:
  case TypeKind::String:
    LOG({
      fmt::print("[CODEGEN] Logging IfExprAST's bool expr \n");
      ASTPrinter::print(ast->bool_expr.get());
    });
    this->abort("Invalid syntax or broken typechecker for now\n");
    break;
  case TypeKind::Bool:
    ast->bool_expr->val = resPtr->Builder->CreateFCmpONE(
        resPtr->Builder->CreateSIToFP(
            ast->bool_expr->val, llvm::Type::getDoubleTy(*resPtr->Context)),
        llvm::ConstantFP::get(*resPtr->Context, llvm::APFloat(0.0)),
        "ifcond_bool");
    break;
  }

  llvm::Function *function = resPtr->Builder->GetInsertBlock()->getParent();

  // Create blocks for the then and else cases
  BasicBlock *ThenBB = BasicBlock::Create(*resPtr->Context, "then", function);
  BasicBlock *ElseBB = BasicBlock::Create(*resPtr->Context, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*resPtr->Context, "ifcont");

  resPtr->Builder->CreateCondBr(ast->bool_expr->val, ThenBB, ElseBB);

  auto blockBranchHelper = [this, &MergeBB](BasicBlock *bb,
                                            BlockAST *block_ast) {
    resPtr->Builder->SetInsertPoint(bb);
    block_ast->accept_vis(this);

    llvm::Value *V = nullptr;
    if (!block_ast->Statements.empty()) {
      V = block_ast->Statements.back()->val;
    }
    BasicBlock *end_bb = resPtr->Builder->GetInsertBlock();
    // Get the actual block we're in now (codegen might have created new blocks)
    if (!end_bb->getTerminator())
      resPtr->Builder->CreateBr(MergeBB);

    return std::pair{V, end_bb};
  };

  auto [ThenV, ThenEndBB] = blockBranchHelper(ThenBB, ast->thenBlockAST.get());

  // Codegen else block
  function->insert(function->end(), ElseBB);

  auto [ElseV, ElseEndBB] = blockBranchHelper(ElseBB, ast->elseBlockAST.get());

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

void CgVisitor::visit(DerefExprAST *ast) {
  ast->walk_with_preorder(this);
  ast->operand->accept_vis(this);
  ast->walk_with_postorder(this);
}

void CgVisitor::postorder_walk(DerefExprAST *ast) {
  auto pointee_type = std::get<PointerType>(ast->operand->type.type_data).get_pointee();
  ast->val = resPtr->Builder->CreateLoad(
      type_converter.get_type(pointee_type), ast->operand->val, "deref");
}

void CgVisitor::postorder_walk(AllocExprAST *ast) {
  auto operand_val = ast->operand->val;
  auto operand_llvm_type = type_converter.get_type(ast->operand->type);

  // Compute sizeof(T) using DataLayout
  auto &data_layout = resPtr->Module->getDataLayout();
  auto size = data_layout.getTypeAllocSize(operand_llvm_type);
  auto *size_val = llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(*resPtr->Context), size);

  // Call malloc(size)
  auto *malloc_fn = resPtr->Module->getFunction("malloc");
  auto *malloc_ptr = resPtr->Builder->CreateCall(malloc_fn, {size_val}, "alloc_ptr");

  // Store the operand value through the opaque pointer
  resPtr->Builder->CreateStore(operand_val, malloc_ptr);

  ast->val = malloc_ptr;
}

void CgVisitor::postorder_walk(FreeExprAST *ast) {
  auto *ptr_val = ast->operand->val;

  // Call free(ptr)
  auto *free_fn = resPtr->Module->getFunction("free");
  resPtr->Builder->CreateCall(free_fn, {ptr_val});

  ast->val = nullptr;
}

void CgVisitor::visit(AddrOfExprAST *ast) {
  ast->walk_with_preorder(this);
  // Don't visit operand normally - we need the alloca, not the loaded value
  auto *var_expr = dynamic_cast<VariableExprAST *>(ast->operand.get());
  this->abort_if_not(var_expr, "Address-of (&) requires a variable operand");
  auto *alloca = this->allocaValues.top()[var_expr->variableName];
  this->abort_if_not(alloca, "Unknown variable in address-of expression");
  ast->val = alloca;
  ast->walk_with_postorder(this);
}

} // namespace sammine_lang::AST
