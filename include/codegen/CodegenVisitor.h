//
// Created by Jasmine Tang on 3/27/24.
//

#pragma once
#include "TypeConverter.h"
#include "ast/AstBase.h"
#include "ast/AstDecl.h"
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

  std::string module_name;
  bool in_reuse = false;
  bool current_func_exported = false;
  TypeConverter type_converter;

  std::map<std::string, llvm::Function *> closure_wrappers;
  size_t partial_counter = 0;
  llvm::Function *getOrCreateClosureWrapper(llvm::Function *fn,
                                            const FunctionType &ft);

  void emitBoundsCheck(llvm::Value *idx, size_t arr_size);
  llvm::Value *emitArrayComparison(llvm::Value *L, llvm::Value *R,
                                   const Type &arrType, TokenType tok);
  llvm::Value *buildClosure(llvm::Value *codePtr, llvm::Value *envPtr);
  llvm::Value *emitArrayElementGEP(llvm::Value *base, llvm::Value *idx,
                                   llvm::Type *arrLlvmType, size_t arrSize);

  void emitCall(ExprAST *ast, llvm::FunctionCallee callee,
                llvm::ArrayRef<llvm::Value *> args, const llvm::Twine &name);
  void emitPartialApplication(CallExprAST *ast, llvm::Function *callee,
                              llvm::ArrayRef<llvm::Value *> boundArgs);
  void emitEnumConstructor(CallExprAST *ast);
  void forward_declare(PrototypeAST *ast);

  // INFO: The collector is named Jasmine because she said on her discord status
  // once that she's a garbage woman lol

public:
  CgVisitor(std::shared_ptr<sammine_lang::LLVMRes> resPtr,
            std::string module_name = "")
      : resPtr(resPtr), module_name(std::move(module_name)),
        type_converter(*resPtr) {}

  void enter_new_scope() override;
  void exit_new_scope() override;

  virtual void visit(FuncDefAST *) override;
  virtual void visit(IfExprAST *) override;
  // visit
  // pre order
  // TODO: Implement these
  virtual void preorder_walk(ProgramAST *ast) override;
  virtual void preorder_walk(VarDefAST *ast) override;
  virtual void preorder_walk(FuncDefAST *ast) override;
  virtual void preorder_walk(StructDefAST *ast) override;
  virtual void preorder_walk(EnumDefAST *ast) override;
  virtual void preorder_walk(ExternAST *ast) override;
  virtual void preorder_walk(PrototypeAST *ast) override;
  virtual void preorder_walk(CallExprAST *ast) override;
  virtual void preorder_walk(ReturnExprAST *ast) override {}
  virtual void preorder_walk(BinaryExprAST *ast) override;
  virtual void preorder_walk(NumberExprAST *ast) override;
  virtual void preorder_walk(StringExprAST *ast) override;
  virtual void preorder_walk(BoolExprAST *ast) override;
  virtual void preorder_walk(CharExprAST *ast) override;
  virtual void preorder_walk(VariableExprAST *ast) override;
  virtual void preorder_walk(BlockAST *ast) override;
  virtual void preorder_walk(IfExprAST *ast) override;
  virtual void preorder_walk(UnitExprAST *ast) override;
  virtual void preorder_walk(TypedVarAST *ast) override;
  virtual void preorder_walk(DerefExprAST *ast) override {}
  virtual void preorder_walk(AddrOfExprAST *ast) override {}
  virtual void preorder_walk(AllocExprAST *ast) override {}
  virtual void preorder_walk(FreeExprAST *ast) override {}
  virtual void preorder_walk(ArrayLiteralExprAST *ast) override {}
  virtual void preorder_walk(IndexExprAST *ast) override {}
  virtual void preorder_walk(LenExprAST *ast) override {}
  virtual void preorder_walk(UnaryNegExprAST *ast) override {}
  virtual void preorder_walk(StructLiteralExprAST *ast) override {}
  virtual void preorder_walk(FieldAccessExprAST *ast) override {}
  virtual void preorder_walk(CaseExprAST *ast) override {}
  virtual void preorder_walk(WhileExprAST *ast) override {}
  virtual void preorder_walk(TypeClassDeclAST *ast) override {}
  virtual void preorder_walk(TypeClassInstanceAST *ast) override {}

  // post order
  // TODO: Implement these?
  virtual void postorder_walk(ProgramAST *ast) override {}
  virtual void postorder_walk(VarDefAST *ast) override;
  virtual void postorder_walk(ExternAST *ast) override;
  virtual void postorder_walk(FuncDefAST *ast) override;
  virtual void postorder_walk(StructDefAST *ast) override;
  virtual void postorder_walk(EnumDefAST *ast) override;
  virtual void postorder_walk(PrototypeAST *ast) override {}
  virtual void postorder_walk(CallExprAST *ast) override;
  virtual void postorder_walk(ReturnExprAST *ast) override;
  virtual void postorder_walk(BinaryExprAST *ast) override;
  virtual void postorder_walk(NumberExprAST *ast) override {}
  virtual void postorder_walk(StringExprAST *ast) override;
  virtual void postorder_walk(BoolExprAST *ast) override {}
  virtual void postorder_walk(CharExprAST *ast) override {}
  virtual void postorder_walk(VariableExprAST *ast) override {}
  virtual void postorder_walk(BlockAST *ast) override;
  virtual void postorder_walk(IfExprAST *ast) override {}
  virtual void postorder_walk(UnitExprAST *ast) override {}
  virtual void postorder_walk(TypedVarAST *ast) override {}
  virtual void postorder_walk(DerefExprAST *ast) override;
  virtual void postorder_walk(AddrOfExprAST *ast) override {}
  virtual void postorder_walk(AllocExprAST *ast) override;
  virtual void postorder_walk(FreeExprAST *ast) override;
  virtual void postorder_walk(ArrayLiteralExprAST *ast) override;
  virtual void postorder_walk(IndexExprAST *ast) override;
  virtual void postorder_walk(LenExprAST *ast) override;
  virtual void postorder_walk(UnaryNegExprAST *ast) override;
  virtual void postorder_walk(StructLiteralExprAST *ast) override;
  virtual void postorder_walk(FieldAccessExprAST *ast) override;
  virtual void postorder_walk(CaseExprAST *ast) override {}
  virtual void postorder_walk(WhileExprAST *ast) override {}
  virtual void postorder_walk(TypeClassDeclAST *ast) override {}
  virtual void postorder_walk(TypeClassInstanceAST *ast) override {}

  virtual void visit(TypeClassDeclAST *ast) override {}
  virtual void visit(TypeClassInstanceAST *ast) override;
  virtual void visit(WhileExprAST *ast) override;
  virtual void visit(DerefExprAST *ast) override;
  virtual void visit(IndexExprAST *ast) override;
  virtual void visit(AddrOfExprAST *ast) override;
  virtual void visit(CaseExprAST *ast) override;
};
} // namespace sammine_lang::AST
