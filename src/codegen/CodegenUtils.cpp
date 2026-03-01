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
  return ast->Prototype->functionName.get_name() == "main";
}
bool CodegenUtils::hasFunctionMain(ProgramAST *ast) {
  for (auto &def : ast->DefinitionVec)
    if (auto func_def = llvm::dyn_cast<FuncDefAST>(def.get()))
      if (isFunctionMain(func_def))
        return true;

  return false;
}

llvm::FunctionCallee CodegenUtils::declare_malloc(llvm::Module &module) {
  llvm::PointerType *int8ptr = llvm::PointerType::get(
      module.getContext(), 0); // 0 stands for generic address space
  return declare_fn(module, "malloc", int8ptr,
                    {llvm::Type::getInt64Ty(module.getContext())});
}

llvm::FunctionCallee CodegenUtils::declare_free(llvm::Module &module) {
  return declare_fn(module, "free", llvm::Type::getVoidTy(module.getContext()),
                    {llvm::PointerType::get(module.getContext(), 0)});
}

llvm::FunctionCallee CodegenUtils::declare_fn(
    llvm::Module &module, const std::string &name, llvm::Type *return_type,
    llvm::ArrayRef<llvm::Type *> param_types, bool is_vararg) {
  llvm::FunctionType *fn_type =
      llvm::FunctionType::get(return_type, param_types, is_vararg);
  return module.getOrInsertFunction(name, fn_type);
}
} // namespace sammine_lang
