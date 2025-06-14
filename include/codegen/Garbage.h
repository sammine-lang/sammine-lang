
#pragma once
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "ast/Ast.h"
#include "ast/AstDecl.h"
#include "codegen/LLVMRes.h"
#include <cstdint>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/GlobalVariable.h>
#include <memory>
#include <string>
#include <string_view>

//! \file Garbage.h
//! \brief Houses ShadowGarbageCollector scheme as well as the ref counter
//
namespace sammine_lang::AST {
class NumRootCalculator {
public:
  static int32_t calculate(BlockAST *ast);
  static int32_t calculate(ExprAST *ast);
  static int32_t calculate(IfExprAST *ast);
  static int32_t calculate(VariableExprAST *ast);

  // TODO: Tell Jasmine to re-check this
  static int32_t calculate(CallExprAST *ast);

  static int32_t calculate(ReturnExprAST *ast);
  static int32_t calculate(BinaryExprAST *ast);
  static int32_t calculate(BoolExprAST *ast);
  static int32_t calculate(StringExprAST *ast);
  static int32_t calculate(NumberExprAST *ast);
  static int32_t calculate(UnitExprAST *ast);
  static int32_t calculate(VarDefAST *ast);
};
class ShadowGarbageCollector {
  [[maybe_unused]]
  llvm::Module &module;
  llvm::LLVMContext &context;
  [[maybe_unused]]
  llvm::IRBuilder<> &builder;
  std::vector<llvm::Constant *> MetaDataEntries;

  // TODO: work with this in create frame map, rename the void to frame map
  // struct
  // rename to getFrameMap
  std::map<std::string, llvm::GlobalVariable *> fn_name_to_frame_map;
  inline static std::string GLOBAL_ROOT_CHAIN = "global_root_chain";
  inline static std::string FRAME_MAP = "frame_map";

  llvm::StructType *FRAME_MAP_TYPE;
  llvm::StructType *STACK_ENTRY_TYPE;

public:
  ShadowGarbageCollector(LLVMRes &resPtr)
      : module(*resPtr.Module.get()), context(*resPtr.Context.get()),
        builder(*resPtr.Builder) {
    // INFO: Frame map
    FRAME_MAP_TYPE = llvm::StructType::create(context, "frame_map_type");
    auto *int64type =
        llvm::Type::getInt64Ty(context); // 0 stands for generic address space
    FRAME_MAP_TYPE->setBody(int64type);

    // INFO: Stack entry
    STACK_ENTRY_TYPE = llvm::StructType::create(context, "stack_entry_type");
    llvm::PointerType *int64ptr =
        llvm::PointerType::get(llvm::Type::getInt64Ty(context),
                               0); // 0 stands for generic address space
    llvm::ArrayType *root_array = llvm::ArrayType::get(int64ptr, 0);
    // llvm::ArrayType *MetaArrayTy =
    //     llvm::ArrayType::get(int8ptr, MetaDataEntries.size());
    // llvm::Constant *MetaArray =
    //     llvm::ConstantArray::get(MetaArrayTy, MetaDataEntries);
    STACK_ENTRY_TYPE->setBody({int64ptr, int64ptr, root_array});
  }

  virtual std::string llvmStrategy() { return "shadow-stack"; }
  llvm::GlobalVariable *getFrameMapForCallee(FuncDefAST *);
  void applyStrategy(llvm::Function *f);
  void setStackEntry(FuncDefAST *callee, llvm::Function *llvm_callee);
  void relieveStackEntry();

  void initGlobalRootChain();

  void initGCFunc();
};

class RefCounter {
  llvm::Module &module;
  llvm::LLVMContext &context;
  llvm::IRBuilder<> &builder;

  inline static constexpr int REFCNT_SIZE = sizeof(int32_t);

  inline static std::string REFCNT_MALLOC_WRAPPER_NAME =
      "refcnt_malloc_wrapper";

public:
  void declare_refcnt_visitor(bool is_main_file,
                              llvm::StructType *stack_entry_type,
                              llvm::StructType *frame_map_type);
  void declare_malloc_wrapper(bool is_main_file);
  void decrease_refcnt(llvm::Value *);
  void increase_refcnt(llvm::Value *);
  RefCounter(LLVMRes &resPtr)
      : module(*resPtr.Module.get()), context(*resPtr.Context.get()),
        builder(*resPtr.Builder) {}
};
} // namespace sammine_lang::AST
