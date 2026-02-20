
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

  static llvm::FunctionCallee declare_malloc(llvm::Module &);
  static llvm::FunctionCallee declare_free(llvm::Module &);

  static llvm::FunctionCallee declare_fn(llvm::Module &module,
                                         const std::string &name,
                                         llvm::Type *return_type,
                                         llvm::ArrayRef<llvm::Type *> param_types,
                                         bool is_vararg = false);
};

} // namespace sammine_lang
