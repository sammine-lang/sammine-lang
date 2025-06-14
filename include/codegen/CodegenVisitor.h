//
// Created by Jasmine Tang on 3/27/24.
//

#pragma once
#include "TypeConverter.h"
#include "ast/AstBase.h"
#include "ast/AstDecl.h"
#include "codegen/Garbage.h"
#include "codegen/LLVMRes.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

//! \file CodegenVisitor.h
//! \brief Defined CgVisitor, which lowers ASTs to LLVM IR
namespace sammine_lang::AST {
class CgVisitor : public ScopedASTVisitor {

private:
  std::shared_ptr<sammine_lang::LLVMRes> resPtr;
  std::stack<std::map<std::string, llvm::AllocaInst *>> allocaValues;

  llvm::Function *current_func;
  llvm::Function *getCurrentFunction();

  void setCurrentFunction(llvm::Function *);

  TypeConverter type_converter;

  // INFO: The collector is named Jasmine because she said on her discord status
  // once that she's a garbage woman lol
  ShadowGarbageCollector jasmine;
  RefCounter ref_counter;

public:
  CgVisitor(std::shared_ptr<sammine_lang::LLVMRes> resPtr)
      : resPtr(resPtr), type_converter(*resPtr), jasmine(*resPtr),
        ref_counter(*resPtr) {}

  void enter_new_scope() override;
  void exit_new_scope() override;

  virtual void visit(FuncDefAST *) override;
  // visit
  // pre order
  // TODO: Implement these
  virtual void preorder_walk(ProgramAST *ast) override;
  virtual void preorder_walk(VarDefAST *ast) override;
  virtual void preorder_walk(FuncDefAST *ast) override;
  virtual void preorder_walk(RecordDefAST *ast) override;
  virtual void preorder_walk(ExternAST *ast) override;
  virtual void preorder_walk(PrototypeAST *ast) override;
  virtual void preorder_walk(CallExprAST *ast) override;
  virtual void preorder_walk(ReturnExprAST *ast) override {}
  virtual void preorder_walk(BinaryExprAST *ast) override;
  virtual void preorder_walk(NumberExprAST *ast) override;
  virtual void preorder_walk(StringExprAST *ast) override;
  virtual void preorder_walk(BoolExprAST *ast) override;
  virtual void preorder_walk(VariableExprAST *ast) override;
  virtual void preorder_walk(BlockAST *ast) override;
  virtual void preorder_walk(IfExprAST *ast) override;
  virtual void preorder_walk(UnitExprAST *ast) override;
  virtual void preorder_walk(TypedVarAST *ast) override;

  // post order
  // TODO: Implement these?
  virtual void postorder_walk(ProgramAST *ast) override {}
  virtual void postorder_walk(VarDefAST *ast) override;
  virtual void postorder_walk(ExternAST *ast) override {}
  virtual void postorder_walk(FuncDefAST *ast) override;
  virtual void postorder_walk(RecordDefAST *ast) override;
  virtual void postorder_walk(PrototypeAST *ast) override {}
  virtual void postorder_walk(CallExprAST *ast) override {}
  virtual void postorder_walk(ReturnExprAST *ast) override;
  virtual void postorder_walk(BinaryExprAST *ast) override;
  virtual void postorder_walk(NumberExprAST *ast) override {}
  virtual void postorder_walk(StringExprAST *ast) override {}
  virtual void postorder_walk(BoolExprAST *ast) override {}
  virtual void postorder_walk(VariableExprAST *ast) override {}
  virtual void postorder_walk(BlockAST *ast) override;
  virtual void postorder_walk(IfExprAST *ast) override {}
  virtual void postorder_walk(UnitExprAST *ast) override {}
  virtual void postorder_walk(TypedVarAST *ast) override {}
};
} // namespace sammine_lang::AST
