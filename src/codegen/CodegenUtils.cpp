#include "codegen/CodegenUtils.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include "ast/Ast.h"
namespace sammine_lang {
llvm::AllocaInst *CodegenUtils::CreateEntryBlockAlloca(
    llvm::Function *Function, const std::string &VarName, llvm::Type *type) {

  llvm::IRBuilder<> TmpB(&Function->getEntryBlock(),
                         Function->getEntryBlock().begin());
  return TmpB.CreateAlloca(type, nullptr, VarName);
}
bool CodegenUtils::isFunctionMain(FuncDefAST *ast) {
  return ast->Prototype->functionName == "main";
}
bool CodegenUtils::hasFunctionMain(ProgramAST *ast) {
  for (auto &def : ast->DefinitionVec)
    if (auto func_def = dynamic_cast<FuncDefAST *>(def.get()))
      if (isFunctionMain(func_def))
        return true;

  return false;
}

llvm::FunctionType *CodegenUtils::declare_malloc(llvm::Module &module) {
  llvm::PointerType *int8ptr =
      llvm::PointerType::get(llvm::Type::getInt8Ty(module.getContext()),
                             0); // 0 stands for generic address space

  // INFO: malloc, since we're a GC language, duhhhh
  llvm::FunctionType *MallocType = llvm::FunctionType::get(
      int8ptr,                                     // return type (i8*)
      llvm::Type::getInt64Ty(module.getContext()), // arg: size_t (i64)
      false);                                      // not variadic
  module.getOrInsertFunction("malloc", MallocType);
  return MallocType;
}
} // namespace sammine_lang
