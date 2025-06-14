
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>

#include "ast/AstDecl.h"
#include "codegen/LLVMRes.h"
namespace sammine_lang {
using namespace AST;
class CodegenUtils {

public:
  static llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
                                                  const std::string &VarName,
                                                  llvm::Type *);

  static bool isFunctionMain(FuncDefAST *);
  static bool hasFunctionMain(ProgramAST *);

  static llvm::FunctionType *declare_malloc(llvm::Module &);
};

class CodegenCommenter {

public:
  CodegenCommenter(LLVMRes &resPtr) {}
};
} // namespace sammine_lang
