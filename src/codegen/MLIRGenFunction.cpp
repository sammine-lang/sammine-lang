#include "codegen/MLIRGenImpl.h"

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"

#include "llvm/ADT/SmallVector.h"

#include "fmt/core.h"

namespace sammine_lang {

void MLIRGenImpl::forwardDeclareFunc(AST::PrototypeAST *proto) {
  if (proto->is_generic())
    return;
  auto funcType = buildFuncType(proto);
  auto funcName = mangleName(proto->functionName);
  if (!theModule.lookupSymbol(funcName)) {
    builder.setInsertionPointToEnd(theModule.getBody());
    auto funcOp = mlir::func::FuncOp::create(builder.getUnknownLoc(),
                                               funcName, funcType);
    funcOp.setVisibility(moduleName.empty()
                             ? mlir::SymbolTable::Visibility::Private
                             : mlir::SymbolTable::Visibility::Public);
    theModule.push_back(funcOp);
  }
}

// ===--- Closure helpers ---===

mlir::Value MLIRGenImpl::buildClosure(mlir::Value codePtr, mlir::Value envPtr,
                                      mlir::Location loc) {
  mlir::Value closure =
      mlir::LLVM::UndefOp::create(builder, loc, closureType);
  closure = mlir::LLVM::InsertValueOp::create(
      builder, loc, closure, codePtr, llvm::ArrayRef<int64_t>{0});
  closure = mlir::LLVM::InsertValueOp::create(
      builder, loc, closure, envPtr, llvm::ArrayRef<int64_t>{1});
  return closure;
}

mlir::LLVM::LLVMFunctionType
MLIRGenImpl::getClosureFuncType(const FunctionType &ft) {
  auto retType = ft.get_return_type();
  if (retType.type_kind == TypeKind::Array)
    sammine_util::abort("MLIRGen: closure returning array not supported");

  auto ptrTy = llvmPtrTy();
  llvm::SmallVector<mlir::Type> params;
  params.push_back(ptrTy); // env ptr
  for (auto &p : ft.get_params_types())
    params.push_back(convertType(p));
  if (retType.type_kind == TypeKind::Unit)
    return mlir::LLVM::LLVMFunctionType::get(
        llvmVoidTy(), params);
  return mlir::LLVM::LLVMFunctionType::get(convertType(retType), params);
}

std::string
MLIRGenImpl::getOrCreateClosureWrapper(const std::string &funcName,
                                       const FunctionType &ft) {
  auto it = closureWrappers.find(funcName);
  if (it != closureWrappers.end())
    return it->second;

  auto wrapperName = kWrapperPrefix.str() + funcName;
  auto wrapperFnTy = getClosureFuncType(ft);
  auto wrapperLoc = builder.getUnknownLoc();

  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToEnd(theModule.getBody());

  auto wrapperOp = mlir::LLVM::LLVMFuncOp::create(
      builder, wrapperLoc, wrapperName, wrapperFnTy,
      mlir::LLVM::Linkage::Internal);
  auto &entryBlock = *wrapperOp.addEntryBlock(builder);
  builder.setInsertionPointToStart(&entryBlock);

  // Forward all args except env (arg 0) to the original func.func
  llvm::SmallVector<mlir::Value> forwardArgs;
  for (size_t i = 1; i < entryBlock.getNumArguments(); i++)
    forwardArgs.push_back(entryBlock.getArgument(i));

  emitFuncCallAndLLVMReturn(funcName, ft.get_return_type(), forwardArgs,
                            wrapperLoc);

  closureWrappers[funcName] = wrapperName;
  return wrapperName;
}

void MLIRGenImpl::emitFuncCallAndLLVMReturn(llvm::StringRef callee,
                                             const Type &retType,
                                             mlir::ValueRange args,
                                             mlir::Location location) {
  if (retType.type_kind == TypeKind::Unit) {
    mlir::func::CallOp::create(builder, location, callee,
                                mlir::TypeRange{}, args);
    mlir::LLVM::ReturnOp::create(builder, location, mlir::ValueRange{});
  } else {
    auto callOp = mlir::func::CallOp::create(
        builder, location, callee,
        mlir::TypeRange{convertType(retType)}, args);
    mlir::LLVM::ReturnOp::create(builder, location, callOp.getResults());
  }
}

// ===--- Function emission ---===

void MLIRGenImpl::emitFunction(AST::FuncDefAST *ast) {
  // Skip generic function templates — only monomorphized copies
  if (ast->Prototype->is_generic())
    return;

  auto location = loc(ast);
  symbolTable.push_context();

  auto funcType = buildFuncType(ast->Prototype.get());
  std::string funcName = mangleName(ast->Prototype->functionName);

  // Look up forward-declared function, or create a new one
  mlir::func::FuncOp funcOp;
  if (auto existing =
          theModule.lookupSymbol<mlir::func::FuncOp>(funcName)) {
    funcOp = existing;
  } else {
    builder.setInsertionPointToEnd(theModule.getBody());
    funcOp = mlir::func::FuncOp::create(location,
                                          llvm::StringRef(funcName),
                                          funcType);
    theModule.push_back(funcOp);
  }
  // Make main and library-module functions public, everything else private
  if (funcName == "main" || !moduleName.empty())
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Public);
  else
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);

  // Create entry block with arguments
  auto &entryBlock = *funcOp.addEntryBlock();

  builder.setInsertionPointToStart(&entryBlock);

  // Determine if this function returns an array (sret transform)
  bool isSret = false;
  if (ast->Prototype->type.type_kind == TypeKind::Function) {
    auto &ft = std::get<FunctionType>(ast->Prototype->type.type_data);
    isSret = ft.get_return_type().type_kind == TypeKind::Array;
  }

  // Bind parameters in symbol table
  // All non-array params get llvm.alloca + llvm.store (uniform model)
  for (size_t i = 0; i < ast->Prototype->parameterVectors.size(); ++i) {
    auto &param = ast->Prototype->parameterVectors[i];
    auto argVal = entryBlock.getArgument(i);
    if (mlir::isa<mlir::MemRefType>(argVal.getType())) {
      // Array/sret params: register memref directly
      symbolTable.registerNameT(param->name, argVal);
    } else {
      auto alloca = emitAllocaOne(argVal.getType(), location);
      mlir::LLVM::StoreOp::create(builder, location, argVal, alloca);
      symbolTable.registerNameT(param->name, alloca);
    }
  }

  // If sret, the last block argument is the output buffer
  if (isSret)
    currentSretBuffer = entryBlock.getArgument(entryBlock.getNumArguments() - 1);

  // Emit the function body
  mlir::Value bodyResult = emitBlock(ast->Block.get());

  // If the block didn't end with a return, insert one.
  // Check if the current block already has a terminator.
  auto *currentBlock = builder.getInsertionBlock();
  if (currentBlock->empty() ||
      !currentBlock->back().hasTrait<mlir::OpTrait::IsTerminator>()) {
    if (isSret && bodyResult) {
      // sret: copy array into output buffer, return void
      mlir::memref::CopyOp::create(builder, location, bodyResult,
                                    currentSretBuffer);
      mlir::func::ReturnOp::create(builder, location);
    } else if (funcType.getNumResults() == 0) {
      mlir::func::ReturnOp::create(builder, location);
    } else if (bodyResult) {
      mlir::func::ReturnOp::create(builder, location,
                                   mlir::ValueRange{bodyResult});
    } else {
      // If we have a return type but no result, something went wrong —
      // this indicates a compiler bug. Emit a zero/null fallback and abort.
      auto retType = funcType.getResult(0);
      mlir::Value zeroVal;
      if (mlir::isa<mlir::LLVM::LLVMPointerType>(retType))
        zeroVal = mlir::LLVM::ZeroOp::create(builder, location, retType);
      else if (mlir::isa<mlir::FloatType>(retType))
        zeroVal = mlir::arith::ConstantFloatOp::create(
                      builder, location, mlir::cast<mlir::FloatType>(retType),
                      llvm::APFloat(0.0))
                      .getResult();
      else
        zeroVal =
            mlir::arith::ConstantIntOp::create(
                builder, location, 0, retType.getIntOrFloatBitWidth())
                .getResult();
      mlir::func::ReturnOp::create(builder, location,
                                   mlir::ValueRange{zeroVal});
      sammine_util::abort(fmt::format(
          "MLIRGen: non-void function '{}' has no return value — compiler bug",
          funcName));
    }
  }

  currentSretBuffer = nullptr;
  symbolTable.pop_context();
}

void MLIRGenImpl::emitExtern(AST::ExternAST *ast) {
  std::string funcName = ast->Prototype->functionName.mangled();

  // Skip if already declared (e.g. by declareRuntimeFunctions)
  if (theModule.lookupSymbol(funcName))
    return;

  auto location = loc(ast);
  builder.setInsertionPointToEnd(theModule.getBody());

  if (ast->Prototype->is_var_arg) {
    // Vararg C functions must use LLVM dialect (func dialect has no varargs)
    llvm::SmallVector<mlir::Type, 4> paramTypes;
    for (auto &param : ast->Prototype->parameterVectors)
      paramTypes.push_back(convertType(param->type));
    // Return type
    auto &funcTypeData = std::get<FunctionType>(ast->Prototype->type.type_data);
    auto retType = funcTypeData.get_return_type();
    mlir::Type llvmRetType;
    if (retType.type_kind == TypeKind::Unit)
      llvmRetType = llvmVoidTy();
    else
      llvmRetType = convertType(retType);
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(
        llvmRetType, paramTypes, /*isVarArg=*/true);
    mlir::LLVM::LLVMFuncOp::create(builder, location, funcName, fnTy);
  } else {
    auto funcType = buildFuncType(ast->Prototype.get());
    auto funcOp = mlir::func::FuncOp::create(
        location, llvm::StringRef(funcName), funcType);
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
    theModule.push_back(funcOp);
  }
}

// ===--- Call emission ---===

mlir::Value MLIRGenImpl::emitPartialApplication(
    AST::CallExprAST *ast, const std::string &calleeName,
    llvm::ArrayRef<mlir::Value> boundArgs) {
  auto location = loc(ast);
  auto &fullFt =
      std::get<FunctionType>(ast->callee_func_type->type_data);
  auto allParams = fullFt.get_params_types();
  size_t boundCount = boundArgs.size();
  auto ptrTy = llvmPtrTy();

  // 1. Create env struct type and stack-allocate it
  llvm::SmallVector<mlir::Type> envFields;
  for (size_t i = 0; i < boundCount; i++)
    envFields.push_back(convertType(allParams[i]));
  auto envStructTy =
      mlir::LLVM::LLVMStructType::getLiteral(builder.getContext(),
                                              envFields);

  auto envAlloca = emitAllocaOne(envStructTy, location);

  // Store bound args into env
  for (size_t i = 0; i < boundCount; i++) {
    auto gep = mlir::LLVM::GEPOp::create(
        builder, location, ptrTy, envStructTy, envAlloca,
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, static_cast<int32_t>(i)});
    mlir::LLVM::StoreOp::create(builder, location, boundArgs[i], gep);
  }

  // 2. Generate partial wrapper: llvm.func @__partial_N(ptr %env, remaining...)
  auto retType = fullFt.get_return_type();
  llvm::SmallVector<mlir::Type> wrapperParams;
  wrapperParams.push_back(ptrTy); // env
  for (size_t i = boundCount; i < allParams.size(); i++)
    wrapperParams.push_back(convertType(allParams[i]));

  mlir::Type llvmRet =
      retType.type_kind == TypeKind::Unit
          ? mlir::Type(
                llvmVoidTy())
          : convertType(retType);
  auto wrapperFnTy =
      mlir::LLVM::LLVMFunctionType::get(llvmRet, wrapperParams);
  auto wrapperName = fmt::format("{}{}", kPartialPrefix, partialCounter++);

  // 2. Generate the wrapper body in a separate scope so InsertionGuard
  //    restores the insertion point before we build the closure.
  {
    mlir::OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(theModule.getBody());

    auto wrapperOp = mlir::LLVM::LLVMFuncOp::create(
        builder, location, wrapperName, wrapperFnTy,
        mlir::LLVM::Linkage::Internal);
    auto &entryBlock = *wrapperOp.addEntryBlock(builder);
    builder.setInsertionPointToStart(&entryBlock);

    // Load bound args from env
    auto envArg = entryBlock.getArgument(0);
    llvm::SmallVector<mlir::Value> fullArgs;
    for (size_t i = 0; i < boundCount; i++) {
      auto gep = mlir::LLVM::GEPOp::create(
          builder, location, ptrTy, envStructTy, envArg,
          llvm::ArrayRef<mlir::LLVM::GEPArg>{0, static_cast<int32_t>(i)});
      auto loaded = mlir::LLVM::LoadOp::create(builder, location,
                                                 envFields[i], gep);
      fullArgs.push_back(loaded);
    }
    // Forward remaining args
    for (size_t i = 1; i < entryBlock.getNumArguments(); i++)
      fullArgs.push_back(entryBlock.getArgument(i));

    // Call original function and return
    emitFuncCallAndLLVMReturn(calleeName, retType, fullArgs, location);
  }

  // 3. Build closure: { wrapper_addr, env_alloca }
  auto wrapperAddr = mlir::LLVM::AddressOfOp::create(builder, location,
                                                       ptrTy, wrapperName);
  return buildClosure(wrapperAddr, envAlloca, location);
}

mlir::Value
MLIRGenImpl::emitIndirectCall(AST::CallExprAST *ast,
                              llvm::ArrayRef<mlir::Value> operands) {
  auto location = loc(ast);
  auto varName = ast->functionName.mangled();

  // Load closure value from symbol table
  auto closureVal = symbolTable.get_from_name(varName);

  // If stored in llvm.alloca (variable), load it first
  if (mlir::isa<mlir::LLVM::LLVMPointerType>(closureVal.getType()))
    closureVal = mlir::LLVM::LoadOp::create(builder, location,
                                              closureType, closureVal);

  // Extract code_ptr and env_ptr
  auto ptrTy = llvmPtrTy();
  auto codePtr = mlir::LLVM::ExtractValueOp::create(
      builder, location, ptrTy, closureVal,
      llvm::ArrayRef<int64_t>{0});
  auto envPtr = mlir::LLVM::ExtractValueOp::create(
      builder, location, ptrTy, closureVal,
      llvm::ArrayRef<int64_t>{1});

  // Build call args: env_ptr, then user args
  llvm::SmallVector<mlir::Value> callArgs;
  callArgs.push_back(envPtr);
  callArgs.append(operands.begin(), operands.end());

  auto &ft =
      std::get<FunctionType>(ast->callee_func_type->type_data);
  auto closureFnTy = getClosureFuncType(ft);

  // For indirect LLVM calls, build the op manually:
  // the callee (function pointer) goes as the first operand,
  // followed by the actual arguments.
  llvm::SmallVector<mlir::Value> allOps;
  allOps.push_back(codePtr);
  allOps.append(callArgs.begin(), callArgs.end());

  auto retType = ft.get_return_type();
  llvm::SmallVector<mlir::Type> resultTypes;
  if (retType.type_kind != TypeKind::Unit)
    resultTypes.push_back(convertType(retType));

  // Indirect call: function pointer is first operand, no callee symbol
  // Use the (LLVMFunctionType, ValueRange) overload for indirect calls
  auto callOp = mlir::LLVM::CallOp::create(
      builder, location, closureFnTy, allOps);
  if (retType.type_kind == TypeKind::Unit)
    return nullptr;
  return callOp.getResult();
}

mlir::Value MLIRGenImpl::emitCallExpr(AST::CallExprAST *ast) {
  // Enum constructor — handled separately
  if (ast->is_enum_constructor)
    return emitEnumConstructor(ast);

  auto location = loc(ast);

  // Emit arguments
  llvm::SmallVector<mlir::Value, 4> operands;
  for (auto &arg : ast->arguments) {
    auto val = emitExpr(arg.get());
    if (val)
      operands.push_back(val);
  }

  // Resolve callee name — use the qualified name directly, matching
  // the LLVM backend. Names already include module prefixes where needed
  // (e.g. "math$add" for imports, "wrapper" for local calls).
  std::string callee;
  if (ast->resolved_generic_name.has_value())
    callee = *ast->resolved_generic_name;
  else
    callee = ast->functionName.mangled();

  // Imported C externs are declared with their base name (e.g. "printf")
  if (!theModule.lookupSymbol(callee)) {
    if (theModule.lookupSymbol(ast->functionName.name))
      callee = ast->functionName.name;
  }

  // Path 1: Direct call (not partial) — callee found in module
  if (theModule.lookupSymbol(callee) && !ast->is_partial) {
    // sret: if call returns an array, allocate local buffer, pass as extra arg
    if (ast->type.type_kind == TypeKind::Array) {
      auto arrMlirType = mlir::cast<mlir::MemRefType>(convertType(ast->type));
      auto localBuf = mlir::memref::AllocaOp::create(
          builder, location, arrMlirType);
      operands.push_back(localBuf);
      mlir::func::CallOp::create(
          builder, location, llvm::StringRef(callee),
          mlir::TypeRange{}, operands);
      return localBuf;
    }

    llvm::SmallVector<mlir::Type, 1> resultTypes;
    if (ast->type.type_kind != TypeKind::Unit)
      resultTypes.push_back(convertType(ast->type));

    // If callee is an llvm.func (C function), use llvm.call
    if (auto llvmFunc =
            theModule.lookupSymbol<mlir::LLVM::LLVMFuncOp>(callee)) {
      auto llvmCallOp = mlir::LLVM::CallOp::create(
          builder, location, resultTypes, callee, operands);
      // Only set var_callee_type for variadic functions
      if (llvmFunc.getFunctionType().isVarArg())
        llvmCallOp.setVarCalleeType(llvmFunc.getFunctionType());
      if (llvmCallOp.getNumResults() > 0)
        return llvmCallOp.getResult();
      return nullptr;
    }

    auto callOp = mlir::func::CallOp::create(
        builder, location, llvm::StringRef(callee), resultTypes, operands);
    if (callOp.getNumResults() > 0)
      return callOp.getResult(0);
    return nullptr;
  }

  // Path 2: Partial application — callee found + is_partial
  if (theModule.lookupSymbol(callee) && ast->is_partial)
    return emitPartialApplication(ast, callee, operands);

  // Path 3: Indirect call through closure variable
  return emitIndirectCall(ast, operands);
}

mlir::Value MLIRGenImpl::emitReturnExpr(AST::ReturnExprAST *ast) {
  auto location = loc(ast);

  if (ast->return_expr) {
    auto val = emitExpr(ast->return_expr.get());
    if (currentSretBuffer && val) {
      // sret: copy array into output buffer, return void
      mlir::memref::CopyOp::create(builder, location, val,
                                    currentSretBuffer);
      mlir::func::ReturnOp::create(builder, location);
    } else if (val) {
      mlir::func::ReturnOp::create(builder, location,
                                   mlir::ValueRange{val});
    } else {
      mlir::func::ReturnOp::create(builder, location);
    }
  } else {
    mlir::func::ReturnOp::create(builder, location);
  }

  return nullptr;
}

} // namespace sammine_lang
