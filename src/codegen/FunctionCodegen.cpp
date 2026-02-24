#include "ast/Ast.h"
#include "codegen/CodegenUtils.h"
#include "codegen/CodegenVisitor.h"
#include "fmt/format.h"
#include "llvm/IR/GlobalIFunc.h"

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
void CgVisitor::forward_declare(PrototypeAST *ast) {
  if (ast->is_generic())
    return;

  std::string llvm_name = ast->functionName;

  if (resPtr->Module->getFunction(llvm_name))
    return;

  std::vector<llvm::Type *> param_types;
  for (auto &p : ast->parameterVectors)
    param_types.push_back(type_converter.get_type(p->type));

  llvm::FunctionType *FT = llvm::FunctionType::get(
      this->type_converter.get_return_type(ast->type), param_types,
      ast->is_var_arg);
  auto *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage,
                                   llvm_name, resPtr->Module.get());

  // Set parameter names so preorder_walk(FuncDefAST) can create named allocas
  size_t i = 0;
  for (auto &arg : F->args())
    arg.setName(ast->parameterVectors[i++]->name);
}

/// INFO: Register the function with its arguments, put it in the module
/// this comes before visiting a function
void CgVisitor::preorder_walk(PrototypeAST *ast) {
  // Compute the LLVM symbol name: mangle library functions with module$func
  // Externs keep their C names — aliases handle the mangled lookup.
  std::string llvm_name = ast->functionName;
  if (!module_name.empty() && ast->functionName != "main" && !in_reuse &&
      !current_func_exported)
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

  // For 'export let' functions, create an IFunc so importers can call via
  // the mangled name (module$func) while C code uses the plain name.
  if (ast->is_exported && !module_name.empty()) {
    auto *fn = getCurrentFunction();
    std::string mangled = module_name + "$" + ast->Prototype->functionName;

    auto *ptrTy = llvm::PointerType::get(*resPtr->Context, 0);
    auto *resolverTy = llvm::FunctionType::get(ptrTy, false);
    auto *resolver = llvm::Function::Create(
        resolverTy, llvm::Function::InternalLinkage,
        "resolve_" + mangled, resPtr->Module.get());
    auto *entry =
        llvm::BasicBlock::Create(*resPtr->Context, "entry", resolver);

    {
      llvm::IRBuilderBase::InsertPointGuard guard(*resPtr->Builder);
      resPtr->Builder->SetInsertPoint(entry);
      resPtr->Builder->CreateRet(fn);
    }

    llvm::GlobalIFunc::create(fn->getFunctionType(), 0,
                              llvm::GlobalValue::ExternalLinkage,
                              mangled, resolver, resPtr->Module.get());
  }
}

void CgVisitor::preorder_walk(CallExprAST *ast) {}

void CgVisitor::emitCall(ExprAST *ast, llvm::FunctionCallee callee,
                         llvm::ArrayRef<llvm::Value *> args,
                         const llvm::Twine &name) {
  if (callee.getFunctionType()->getReturnType()->isVoidTy())
    resPtr->Builder->CreateCall(callee, args);
  else
    ast->val = resPtr->Builder->CreateCall(callee, args, name);
}

void CgVisitor::emitPartialApplication(CallExprAST *ast,
                                        llvm::Function *callee,
                                        llvm::ArrayRef<llvm::Value *> boundArgs) {
  this->abort_if_not(ast->callee_func_type.has_value(),
                     "ICE: partial call missing callee_func_type");
  auto full_ft = std::get<FunctionType>(ast->callee_func_type->type_data);
  auto all_params = full_ft.get_params_types();
  size_t bound_count = boundArgs.size();

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
    resPtr->Builder->CreateStore(boundArgs[i], gep);
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

  {
    llvm::IRBuilderBase::InsertPointGuard guard(*resPtr->Builder);

    auto *entry =
        llvm::BasicBlock::Create(*resPtr->Context, "entry", wrapper);
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
  }

  // Build closure struct: {partial_fn_ptr, env_ptr}
  ast->val = buildClosure(wrapper, envAlloca);
}

void CgVisitor::postorder_walk(CallExprAST *ast) {
  // Collect argument values
  std::vector<llvm::Value *> ArgsVector;
  for (auto &arg : ast->arguments)
    ArgsVector.push_back(arg->val);

  // Path 1: Direct call to a module-level function
  llvm::Function *callee = resPtr->Module->getFunction(ast->functionName.mangled());
  if (callee && !ast->is_partial) {
    LOG({
      fmt::print(stderr, "[codegen] call '{}': direct call with {} args\n",
                 ast->functionName.display(), ast->arguments.size());
    });
    // Skip arg count check for variadic functions (like printf)
    if (!callee->isVarArg() && ast->arguments.size() != callee->arg_size())
      this->abort("Incorrect number of arguments passed");

    emitCall(ast, callee, ArgsVector, "calltmp");
    return;
  }

  // Path 2: Partial application of a direct function
  if (callee && ast->is_partial) {
    LOG({
      fmt::print(stderr,
                 "[codegen] call '{}': partial application, binding {} of {} "
                 "args, wrapper = __partial_{}\n",
                 ast->functionName.display(), ast->arguments.size(), callee->arg_size(),
                 partial_counter);
    });
    emitPartialApplication(ast, callee, ArgsVector);
    return;
  }

  // Path 3: Indirect call through a function-typed variable
  auto *alloca = this->allocaValues.top()[ast->functionName.mangled()];
  if (alloca) {
    LOG({
      fmt::print(stderr,
                 "[codegen] call '{}': indirect call through closure with {} "
                 "args\n",
                 ast->functionName.display(), ast->arguments.size());
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

    emitCall(ast, llvm::FunctionCallee(funcTy, codePtr), indirectArgs, "icall");
    return;
  }

  this->abort("Unknown function called");
}
} // namespace sammine_lang::AST
