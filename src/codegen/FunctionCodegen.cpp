#include "ast/Ast.h"
#include "codegen/CodegenUtils.h"
#include "codegen/CodegenVisitor.h"
#include "fmt/format.h"

#define DEBUG_TYPE "codegen"
#include "util/Logging.h"

namespace sammine_lang::AST {

void CgVisitor::preorder_walk(BlockAST *ast) {}
void CgVisitor::postorder_walk(BlockAST *ast) {}
void CgVisitor::preorder_walk(FuncDefAST *ast) {
  auto *Function = this->getCurrentFunction();
  llvm::BasicBlock *mainblock =
      llvm::BasicBlock::Create(*(resPtr->Context), "entry", Function);

  resPtr->Builder->SetInsertPoint(mainblock);

  for (auto &Arg : Function->args()) {
    auto *Alloca = CodegenUtils::CreateEntryBlockAlloca(
        Function, std::string(Arg.getName()), Arg.getType());
    resPtr->Builder->CreateStore(&Arg, Alloca);

    this->allocaValues.top()[std::string(Arg.getName())] = Alloca;
  }
}
/// INFO: Register the function with its arguments, put it in the module
/// this comes before visiting a function
void CgVisitor::preorder_walk(PrototypeAST *ast) {
  // Compute the LLVM symbol name: mangle library functions with module$func
  // Externs keep their C names — aliases handle the mangled lookup.
  std::string llvm_name = ast->functionName;
  if (!module_name.empty() && ast->functionName != "main" && !in_extern)
    llvm_name = module_name + "$" + ast->functionName;

  // If the function is already declared in the module (e.g. runtime builtins
  // like printf), reuse the existing declaration instead of creating a
  // duplicate with a potentially different signature.
  if (auto *existing = resPtr->Module->getFunction(llvm_name)) {
    ast->function = existing;
    current_func = existing;
    LOG({
      fmt::print(stderr,
                 "[codegen] reusing existing declaration for '{}'\n",
                 llvm_name);
    });
    return;
  }

  std::vector<llvm::Type *> param_types;
  for (auto &p : ast->parameterVectors)
    param_types.push_back(type_converter.get_type(p->type));

  llvm::FunctionType *FT = llvm::FunctionType::get(
      this->type_converter.get_return_type(ast->type), param_types,
      ast->is_var_arg);
  llvm::Function *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                             llvm_name, resPtr->Module.get());

  size_t param_index = 0;
  auto &vect = ast->parameterVectors;
  for (auto &arg : F->args()) {
    auto &typed_var = vect[param_index];
    arg.setName(typed_var->name);
    // Mark immutable pointer params as readonly nocapture so LLVM can optimize
    if (typed_var->type.type_kind == TypeKind::Pointer &&
        !typed_var->is_mutable) {
      F->addParamAttr(param_index, llvm::Attribute::ReadOnly);
      F->addParamAttr(param_index,
                      llvm::Attribute::getWithCaptureInfo(
                          *resPtr->Context, llvm::CaptureInfo::none()));
    }
    param_index++;
  }
  ast->function = F;
  current_func = F;
  assert(F);
  LOG({
    fmt::print(stderr, "[codegen] register function '{}' with {} params\n",
               ast->functionName, ast->parameterVectors.size());
  });
}

void CgVisitor::postorder_walk(FuncDefAST *ast) {
  auto not_verified = verifyFunction(*getCurrentFunction(), &llvm::errs());
  if (not_verified) {
    resPtr->Module->print(llvm::errs(), nullptr);
    this->abort("ICE: Abort from creating a function");
  }
}

void CgVisitor::preorder_walk(CallExprAST *ast) {}

void CgVisitor::postorder_walk(CallExprAST *ast) {
  // Collect argument values
  std::vector<llvm::Value *> ArgsVector;
  for (auto &arg : ast->arguments)
    ArgsVector.push_back(arg->val);

  // Path 1: Direct call to a module-level function
  llvm::Function *callee = resPtr->Module->getFunction(ast->functionName);
  if (callee && !ast->is_partial) {
    LOG({
      fmt::print(stderr, "[codegen] call '{}': direct call with {} args\n",
                 ast->functionName, ast->arguments.size());
    });
    // Skip arg count check for variadic functions (like printf)
    if (!callee->isVarArg() && ast->arguments.size() != callee->arg_size())
      this->abort("Incorrect number of arguments passed");

    if (callee->getReturnType() != llvm::Type::getVoidTy(*resPtr->Context))
      ast->val = resPtr->Builder->CreateCall(callee, ArgsVector, "calltmp");
    else
      resPtr->Builder->CreateCall(callee, ArgsVector);
    return;
  }

  // Path 2: Partial application of a direct function
  if (callee && ast->is_partial) {
    LOG({
      fmt::print(stderr,
                 "[codegen] call '{}': partial application, binding {} of {} "
                 "args, wrapper = __partial_{}\n",
                 ast->functionName, ast->arguments.size(), callee->arg_size(),
                 partial_counter);
    });
    this->abort_if_not(ast->callee_func_type.has_value(),
                       "ICE: partial call missing callee_func_type");
    auto full_ft = std::get<FunctionType>(ast->callee_func_type->type_data);
    auto all_params = full_ft.get_params_types();
    size_t bound_count = ast->arguments.size();
    // Create env struct type for bound args
    std::vector<llvm::Type *> env_fields;
    for (size_t i = 0; i < bound_count; i++)
      env_fields.push_back(type_converter.get_type(all_params[i]));
    auto *envStructTy = llvm::StructType::get(*resPtr->Context, env_fields);

    // Stack-allocate env and store bound args
    auto *envAlloca = CodegenUtils::CreateEntryBlockAlloca(
        getCurrentFunction(), "partial_env", envStructTy);
    for (size_t i = 0; i < bound_count; i++) {
      auto *gep = resPtr->Builder->CreateStructGEP(envStructTy, envAlloca, i);
      resPtr->Builder->CreateStore(ArgsVector[i], gep);
    }

    // Generate partial wrapper function at module scope
    // Signature: ret_type(ptr %env, remaining_param_types...)
    auto ret_type = full_ft.get_return_type();
    std::vector<llvm::Type *> wrapperParams;
    wrapperParams.push_back(llvm::PointerType::get(*resPtr->Context, 0)); // env
    for (size_t i = bound_count; i < all_params.size(); i++)
      wrapperParams.push_back(type_converter.get_type(all_params[i]));

    llvm::Type *llvm_ret = ret_type.type_kind == TypeKind::Unit
                               ? llvm::Type::getVoidTy(*resPtr->Context)
                               : type_converter.get_type(ret_type);
    auto *wrapperFT = llvm::FunctionType::get(llvm_ret, wrapperParams, false);
    auto wrapperName = fmt::format("__partial_{}", partial_counter++);
    auto *wrapper =
        llvm::Function::Create(wrapperFT, llvm::Function::InternalLinkage,
                               wrapperName, resPtr->Module.get());

    // Save insert point
    auto *savedBB = resPtr->Builder->GetInsertBlock();
    auto savedPt = resPtr->Builder->GetInsertPoint();

    auto *entry = llvm::BasicBlock::Create(*resPtr->Context, "entry", wrapper);
    resPtr->Builder->SetInsertPoint(entry);

    // Load bound args from env
    auto *envArg = &*wrapper->arg_begin();
    std::vector<llvm::Value *> fullArgs;
    for (size_t i = 0; i < bound_count; i++) {
      auto *gep = resPtr->Builder->CreateStructGEP(envStructTy, envArg, i);
      fullArgs.push_back(resPtr->Builder->CreateLoad(env_fields[i], gep));
    }
    // Forward remaining args
    for (auto it = wrapper->arg_begin() + 1; it != wrapper->arg_end(); ++it)
      fullArgs.push_back(&*it);

    if (llvm_ret->isVoidTy()) {
      resPtr->Builder->CreateCall(callee, fullArgs);
      resPtr->Builder->CreateRetVoid();
    } else {
      auto *result =
          resPtr->Builder->CreateCall(callee, fullArgs, "partial_call");
      resPtr->Builder->CreateRet(result);
    }

    // Restore insert point
    resPtr->Builder->SetInsertPoint(savedBB, savedPt);

    // Build closure struct: {partial_fn_ptr, env_ptr}
    auto *closureTy =
        llvm::StructType::getTypeByName(*resPtr->Context, "sammine.closure");
    llvm::Value *closure = llvm::UndefValue::get(closureTy);
    closure =
        resPtr->Builder->CreateInsertValue(closure, wrapper, 0, "pcls.code");
    closure =
        resPtr->Builder->CreateInsertValue(closure, envAlloca, 1, "pcls.env");
    ast->val = closure;
    return;
  }

  // Path 3: Indirect call through a function-typed variable
  auto *alloca = this->allocaValues.top()[ast->functionName];
  if (alloca) {
    LOG({
      fmt::print(stderr,
                 "[codegen] call '{}': indirect call through closure with {} "
                 "args\n",
                 ast->functionName, ast->arguments.size());
    });
    this->abort_if_not(ast->callee_func_type.has_value(),
                       "ICE: indirect call missing callee_func_type");
    auto ft = std::get<FunctionType>(ast->callee_func_type->type_data);
    auto *closureTy =
        llvm::StructType::getTypeByName(*resPtr->Context, "sammine.closure");
    auto *closureVal =
        resPtr->Builder->CreateLoad(closureTy, alloca, "closure");
    auto *codePtr =
        resPtr->Builder->CreateExtractValue(closureVal, 0, "code_ptr");
    auto *envPtr =
        resPtr->Builder->CreateExtractValue(closureVal, 1, "env_ptr");

    auto *funcTy = type_converter.get_closure_function_type(ft);

    // Build args: env_ptr, then the user-provided args
    std::vector<llvm::Value *> indirectArgs;
    indirectArgs.push_back(envPtr);
    for (auto &v : ArgsVector)
      indirectArgs.push_back(v);

    if (funcTy->getReturnType()->isVoidTy())
      resPtr->Builder->CreateCall(funcTy, codePtr, indirectArgs);
    else
      ast->val =
          resPtr->Builder->CreateCall(funcTy, codePtr, indirectArgs, "icall");
    return;
  }

  this->abort("Unknown function called");
}
} // namespace sammine_lang::AST
