#include "codegen/MLIRGenImpl.h"
#include "codegen/CABI.h"
#include "codegen/MLIRGenBinaryOps.h"
#include "typecheck/BuiltinInstances.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinTypes.h"

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
    // Private until emitFunction gives it a body and decides who can link to
    // it. One that never gets a body, an imported instance method, must stay
    // private: MLIR doesn't allow a public declaration.
    auto funcOp =
        mlir::func::FuncOp::create(builder.getUnknownLoc(), funcName, funcType);
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
    theModule.push_back(funcOp);
  }
}

// ===--- Calls ---===

// `value` as `type`: itself if it has that type, else through a cast that
// lowering removes once both sides are LLVM types.
static mlir::Value bridge(mlir::OpBuilder &builder, mlir::Location location,
                          mlir::Value value, mlir::Type type) {
  if (value.getType() == type)
    return value;
  return mlir::UnrealizedConversionCastOp::create(builder, location, type,
                                                  value)
      .getResult(0);
}

llvm::SmallVector<mlir::Value, 1>
MLIRGenImpl::emitSammineCall(llvm::StringRef callee, mlir::ValueRange args,
                             mlir::TypeRange resultTypes,
                             mlir::Location location) {
  auto funcOp = theModule.lookupSymbol<mlir::func::FuncOp>(callee);
  if (!funcOp)
    imm_error(fmt::format("no function '{}' to call", callee.str()));
  auto fnType = funcOp.getFunctionType();
  if (fnType.getNumInputs() != args.size() ||
      fnType.getNumResults() != resultTypes.size())
    imm_error(fmt::format("call to '{}' doesn't match its signature",
                          callee.str()));

  llvm::SmallVector<mlir::Value, 4> operands;
  for (auto [arg, param] : llvm::zip(args, fnType.getInputs()))
    operands.push_back(bridge(builder, location, arg, param));
  auto call = mlir::func::CallOp::create(builder, location, funcOp, operands);

  llvm::SmallVector<mlir::Value, 1> results;
  for (auto [result, want] : llvm::zip(call.getResults(), resultTypes))
    results.push_back(bridge(builder, location, result, want));
  return results;
}

// ===--- Closure helpers ---===

mlir::Value MLIRGenImpl::buildClosure(mlir::Value codePtr, mlir::Value envPtr,
                                      mlir::Location loc) {
  mlir::Value closure = mlir::LLVM::PoisonOp::create(builder, loc, closureType);
  closure = mlir::LLVM::InsertValueOp::create(builder, loc, closure, codePtr,
                                              llvm::ArrayRef<int64_t>{0});
  closure = mlir::LLVM::InsertValueOp::create(builder, loc, closure, envPtr,
                                              llvm::ArrayRef<int64_t>{1});
  return closure;
}

mlir::LLVM::LLVMFunctionType
MLIRGenImpl::getClosureFuncType(const FunctionType &ft) {
  auto retType = ft.get_return_type();
  if (retType.type_kind == TypeKind::Array)
    imm_error("closure returning array not supported");

  auto ptrTy = llvmPtrTy();
  llvm::SmallVector<mlir::Type> params;
  params.push_back(ptrTy); // env ptr
  for (auto &p : ft.get_params_types())
    params.push_back(convertType(p));
  if (retType.type_kind == TypeKind::Unit)
    return mlir::LLVM::LLVMFunctionType::get(llvmVoidTy(), params);
  return mlir::LLVM::LLVMFunctionType::get(convertType(retType), params);
}

std::string MLIRGenImpl::getOrCreateClosureWrapper(const std::string &funcName,
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
  for (unsigned i = 1; i < entryBlock.getNumArguments(); i++)
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
  if (auto llvmFunc = theModule.lookupSymbol<mlir::LLVM::LLVMFuncOp>(callee)) {
    auto result = emitCCall(llvmFunc, {args.begin(), args.end()}, retType,
                            location);
    mlir::LLVM::ReturnOp::create(builder, location,
                                 result ? mlir::ValueRange{result}
                                        : mlir::ValueRange{});
    return;
  }
  llvm::SmallVector<mlir::Type, 1> resultTypes;
  if (retType.type_kind != TypeKind::Unit)
    resultTypes.push_back(convertType(retType));
  auto results = emitSammineCall(callee, args, resultTypes, location);
  mlir::LLVM::ReturnOp::create(builder, location, results);
}

// ===--- Function emission ---===

void MLIRGenImpl::emitFunction(AST::FuncDefAST *ast, Linkage linkage) {
  auto location = loc(ast);
  decltype(symbolTable)::Guard scope(symbolTable);

  auto funcType = buildFuncType(ast->Prototype.get());
  std::string funcName = mangleName(ast->Prototype->functionName);

  // Look up forward-declared function, or create a new one
  mlir::func::FuncOp funcOp;
  if (auto existing = theModule.lookupSymbol<mlir::func::FuncOp>(funcName)) {
    funcOp = existing;
  } else {
    builder.setInsertionPointToEnd(theModule.getBody());
    funcOp = mlir::func::FuncOp::create(location, llvm::StringRef(funcName),
                                        funcType);
    theModule.push_back(funcOp);
  }
  // MLIR visibility doesn't reach LLVM, so set the linkage too.
  if (funcName == "main")
    linkage = Linkage::Global;
  funcOp.setVisibility(linkage == Linkage::Internal
                           ? mlir::SymbolTable::Visibility::Private
                           : mlir::SymbolTable::Visibility::Public);
  if (linkage != Linkage::Global)
    funcOp->setAttr("llvm.linkage",
                    mlir::LLVM::LinkageAttr::get(
                        builder.getContext(),
                        linkage == Linkage::Internal
                            ? mlir::LLVM::Linkage::Internal
                            : mlir::LLVM::Linkage::AvailableExternally));

  // Its body came through MIR (lowerThroughMIR); only its linkage is set
  // here.
  ++functionCount;
  if (funcOp->hasAttr(MIRLowering::kMirAttr))
    return;

  // Create entry block with arguments
  auto &entryBlock = *funcOp.addEntryBlock();

  builder.setInsertionPointToStart(&entryBlock);

  // Determine if this function returns an array (sret transform)
  bool isSret = false;
  if (ast->Prototype->get_type().type_kind == TypeKind::Function) {
    auto ft = std::get<FunctionType>(ast->Prototype->get_type().type_data);
    isSret = ft.get_return_type().type_kind == TypeKind::Array;
  }

  // Bind parameters in symbol table
  // All params get llvm.alloca + llvm.store (uniform model)
  for (size_t i = 0; i < ast->Prototype->parameterVectors.size(); ++i) {
    auto &param = ast->Prototype->parameterVectors[i];
    auto argVal = entryBlock.getArgument(static_cast<unsigned>(i));
    auto alloca = emitAllocaOne(argVal.getType(), location);
    mlir::LLVM::StoreOp::create(builder, location, argVal, alloca);
    symbolTable.registerNameT(param->name, alloca);
  }

  // If sret, the last block argument is the output buffer
  if (isSret)
    currentSretBuffer =
        entryBlock.getArgument(entryBlock.getNumArguments() - 1);

  // Emit the function body
  mlir::Value bodyResult = emitBlock(ast->Block.get());

  // If the block didn't end with a return, insert one.
  // Check if the current block already has a terminator.
  auto *currentBlock = builder.getInsertionBlock();
  if (currentBlock->empty() ||
      !currentBlock->back().hasTrait<mlir::OpTrait::IsTerminator>()) {
    if (isSret && bodyResult) {
      // sret: load whole array from source, store into output buffer
      auto arrMlirType = convertType(
          std::get<FunctionType>(ast->Prototype->get_type().type_data)
              .get_return_type());
      auto arrVal = mlir::LLVM::LoadOp::create(builder, location, arrMlirType,
                                               bodyResult);
      mlir::LLVM::StoreOp::create(builder, location, arrVal, currentSretBuffer);
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
        zeroVal = mlir::arith::ConstantIntOp::create(
                      builder, location, 0, retType.getIntOrFloatBitWidth())
                      .getResult();
      mlir::func::ReturnOp::create(builder, location,
                                   mlir::ValueRange{zeroVal});
      imm_error(fmt::format(
                    "non-void function '{}' has no return value — compiler bug",
                    funcName),
                ast->get_location());
    }
  }

  currentSretBuffer = nullptr;
}

namespace {

// C expects a char or bool argument or result already extended to 32 bits
// by whoever produces it: signed for char, unsigned for bool. LLVM extends
// it when the declaration says so.
std::optional<llvm::StringRef> cExtension(const Type &type) {
  if (type.type_kind == TypeKind::Char)
    return mlir::LLVM::LLVMDialect::getSExtAttrName();
  if (type.type_kind == TypeKind::Bool)
    return mlir::LLVM::LLVMDialect::getZExtAttrName();
  return std::nullopt;
}

// Marks the char and bool parameters and result of a C function. Its
// declared parameters start at `firstParam`, after a hidden sret pointer.
template <typename FuncOp>
void markCExtensions(FuncOp funcOp, AST::PrototypeAST *proto,
                     unsigned firstParam) {
  auto *ctx = funcOp.getContext();
  auto &params = proto->parameterVectors;
  for (unsigned i = 0; i < params.size(); i++)
    if (auto ext = cExtension(params[i]->get_type()))
      funcOp.setArgAttr(firstParam + i, *ext, mlir::UnitAttr::get(ctx));
  auto retType =
      std::get<FunctionType>(proto->get_type().type_data).get_return_type();
  if (auto ext = cExtension(retType))
    funcOp.setResultAttr(0, *ext, mlir::UnitAttr::get(ctx));
}

} // namespace

void MLIRGenImpl::emitExtern(AST::ExternAST *ast) {
  // A C binding's symbol is its C name in every module that declares it:
  // std::printf is printf. Calls find it through c_symbols.
  std::string funcName = ast->Prototype->functionName.mangled();
  if (ast->is_c_function) {
    c_symbols[funcName] = ast->Prototype->functionName.get_name();
    funcName = ast->Prototype->functionName.get_name();
  }

  // Skip if already declared (e.g. by declareRuntimeFunctions, or by another
  // module's binding of the same C function)
  if (theModule.lookupSymbol(funcName))
    return;

  auto location = loc(ast);
  builder.setInsertionPointToEnd(theModule.getBody());

  // C moves a struct in registers, as the type cabi::registerType gives, or
  // through memory: a returned one through a pointer the caller passes as a
  // hidden first argument (sret), an argument as a pointer to a copy
  // (byval). An argument struct also goes through memory when the registers
  // it needs are no longer all free. LLVM leaves this to the frontend.
  auto retType = std::get<FunctionType>(ast->Prototype->get_type().type_data)
                     .get_return_type();
  auto &params = ast->Prototype->parameterVectors;
  auto is_struct = [](const Type &type) {
    return type.type_kind == TypeKind::Struct ||
           type.type_kind == TypeKind::Tuple;
  };
  if (ast->is_c_function &&
      (is_struct(retType) || llvm::any_of(params, [&](auto &param) {
         return is_struct(param->get_type());
       }))) {
    mlir::DataLayout layout(theModule);
    llvm::SmallVector<mlir::Type, 4> paramTypes;
    mlir::Type cRetType = llvmVoidTy();
    mlir::Type sretType;
    if (is_struct(retType)) {
      auto structTy = convertType(retType);
      if (auto regTy = cabi::registerType(structTy, layout)) {
        cRetType = regTy;
      } else {
        sretType = structTy;
        paramTypes.push_back(llvmPtrTy());
      }
    } else if (retType.type_kind != TypeKind::Unit) {
      cRetType = convertType(retType);
    }

    // Registers still free for arguments; the sret pointer takes one.
    cabi::Registers free{cabi::kIntegerArgRegisters,
                         cabi::kFloatArgRegisters};
    if (sretType)
      free.integer--;
    auto take = [&](const cabi::Registers &needed) {
      free.integer -= std::min(free.integer, needed.integer);
      free.floating -= std::min(free.floating, needed.floating);
    };

    llvm::SmallVector<std::pair<unsigned, mlir::Type>> byvalTypes;
    for (auto &param : params) {
      auto paramTy = convertType(param->get_type());
      if (!is_struct(param->get_type())) {
        take(cabi::registersFor(paramTy));
        paramTypes.push_back(paramTy);
        continue;
      }
      auto regTy = cabi::registerType(paramTy, layout);
      auto needed = regTy ? cabi::registersFor(regTy) : cabi::Registers{};
      if (regTy && needed.integer <= free.integer &&
          needed.floating <= free.floating) {
        take(needed);
        paramTypes.push_back(regTy);
      } else {
        byvalTypes.push_back({paramTypes.size(), paramTy});
        paramTypes.push_back(llvmPtrTy());
      }
    }

    auto fnTy = mlir::LLVM::LLVMFunctionType::get(cRetType, paramTypes,
                                                  ast->Prototype->is_var_arg);
    auto funcOp =
        mlir::LLVM::LLVMFuncOp::create(builder, location, funcName, fnTy);
    if (sretType)
      funcOp.setArgAttr(0, mlir::LLVM::LLVMDialect::getStructRetAttrName(),
                        mlir::TypeAttr::get(sretType));
    for (auto &[index, byvalType] : byvalTypes)
      funcOp.setArgAttr(index, mlir::LLVM::LLVMDialect::getByValAttrName(),
                        mlir::TypeAttr::get(byvalType));
    markCExtensions(funcOp, ast->Prototype.get(), sretType ? 1 : 0);
    return;
  }

  if (ast->Prototype->is_var_arg) {
    // Vararg C functions must use LLVM dialect (func dialect has no varargs)
    llvm::SmallVector<mlir::Type, 4> paramTypes;
    for (auto &param : ast->Prototype->parameterVectors)
      paramTypes.push_back(convertType(param->get_type()));
    mlir::Type llvmRetType;
    if (retType.type_kind == TypeKind::Unit)
      llvmRetType = llvmVoidTy();
    else
      llvmRetType = convertType(retType);
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(llvmRetType, paramTypes,
                                                  /*isVarArg=*/true);
    auto funcOp =
        mlir::LLVM::LLVMFuncOp::create(builder, location, funcName, fnTy);
    if (ast->is_c_function)
      markCExtensions(funcOp, ast->Prototype.get(), 0);
  } else {
    auto funcType = buildFuncType(ast->Prototype.get());
    auto funcOp = mlir::func::FuncOp::create(
        location, llvm::StringRef(funcName), funcType);
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
    theModule.push_back(funcOp);
    if (ast->is_c_function)
      markCExtensions(funcOp, ast->Prototype.get(), 0);
  }
}

// ===--- Call emission ---===

mlir::Value
MLIRGenImpl::emitPartialApplication(AST::CallExprAST *ast,
                                    const std::string &calleeName,
                                    llvm::ArrayRef<mlir::Value> boundArgs) {
  auto location = loc(ast);
  auto &fullFt = std::get<FunctionType>(
      props_.call(ast->id())->callee_func_type->type_data);
  auto allParams = fullFt.get_params_types();
  size_t boundCount = boundArgs.size();
  auto ptrTy = llvmPtrTy();

  // 1. Create env struct type and stack-allocate it
  llvm::SmallVector<mlir::Type> envFields;
  for (size_t i = 0; i < boundCount; i++)
    envFields.push_back(convertType(allParams[i]));
  auto envStructTy =
      mlir::LLVM::LLVMStructType::getLiteral(builder.getContext(), envFields);

  auto envAlloca = emitAllocaOne(envStructTy, location);

  // Store bound args into env
  for (size_t i = 0; i < boundCount; i++) {
    auto gep = mlir::LLVM::GEPOp::create(
        builder, location, ptrTy, envStructTy, envAlloca,
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, static_cast<int32_t>(i)},
        mlir::LLVM::GEPNoWrapFlags::inbounds);
    mlir::LLVM::StoreOp::create(builder, location, boundArgs[i], gep);
  }

  // 2. Generate partial wrapper: llvm.func @__partial_N(ptr %env, remaining...)
  auto retType = fullFt.get_return_type();
  llvm::SmallVector<mlir::Type> wrapperParams;
  wrapperParams.push_back(ptrTy); // env
  for (size_t i = boundCount; i < allParams.size(); i++)
    wrapperParams.push_back(convertType(allParams[i]));

  mlir::Type llvmRet = retType.type_kind == TypeKind::Unit
                           ? mlir::Type(llvmVoidTy())
                           : convertType(retType);
  auto wrapperFnTy = mlir::LLVM::LLVMFunctionType::get(llvmRet, wrapperParams);
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
          llvm::ArrayRef<mlir::LLVM::GEPArg>{0, static_cast<int32_t>(i)},
          mlir::LLVM::GEPNoWrapFlags::inbounds);
      auto loaded =
          mlir::LLVM::LoadOp::create(builder, location, envFields[i], gep);
      fullArgs.push_back(loaded);
    }
    // Forward remaining args
    for (unsigned i = 1; i < entryBlock.getNumArguments(); i++)
      fullArgs.push_back(entryBlock.getArgument(i));

    // Call original function and return
    emitFuncCallAndLLVMReturn(calleeName, retType, fullArgs, location);
  }

  // 3. Build closure: { wrapper_addr, env_alloca }
  auto wrapperAddr =
      mlir::LLVM::AddressOfOp::create(builder, location, ptrTy, wrapperName);
  return buildClosure(wrapperAddr, envAlloca, location);
}

mlir::Value
MLIRGenImpl::emitIndirectCall(AST::CallExprAST *ast,
                              llvm::ArrayRef<mlir::Value> operands) {
  auto location = loc(ast);
  auto varName = ast->functionName.mangled();

  // Load closure value from symbol table
  auto closureVal = symbolTable.recursive_get_from_name(varName);

  // If stored in llvm.alloca (variable), load it first
  if (mlir::isa<mlir::LLVM::LLVMPointerType>(closureVal.getType()))
    closureVal =
        mlir::LLVM::LoadOp::create(builder, location, closureType, closureVal);

  // Extract code_ptr and env_ptr
  auto ptrTy = llvmPtrTy();
  auto codePtr = mlir::LLVM::ExtractValueOp::create(
      builder, location, ptrTy, closureVal, llvm::ArrayRef<int64_t>{0});
  auto envPtr = mlir::LLVM::ExtractValueOp::create(
      builder, location, ptrTy, closureVal, llvm::ArrayRef<int64_t>{1});

  // Build call args: env_ptr, then user args
  llvm::SmallVector<mlir::Value> callArgs;
  callArgs.push_back(envPtr);
  callArgs.append(operands.begin(), operands.end());

  auto &ft = std::get<FunctionType>(
      props_.call(ast->id())->callee_func_type->type_data);
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
  auto callOp =
      mlir::LLVM::CallOp::create(builder, location, closureFnTy, allOps);
  if (retType.type_kind == TypeKind::Unit)
    return nullptr;
  return callOp.getResult();
}

// Calls the C function `llvmFunc` with sammine values, as C passes them: a
// struct argument in its register form or as a byval copy, and a struct
// result in its register form or through an sret buffer. Returns the result
// as `semRetType`, or nullptr for none.
mlir::Value MLIRGenImpl::emitCCall(mlir::LLVM::LLVMFuncOp llvmFunc,
                                   llvm::SmallVector<mlir::Value, 4> operands,
                                   const Type &semRetType,
                                   mlir::Location location) {
  // A struct returned through memory: pass a buffer first, load from it.
  mlir::Value resultBuf;
  if (llvmFunc.getNumArguments() > 0)
    if (auto sretTy = llvmFunc.getArgAttrOfType<mlir::TypeAttr>(
            0, mlir::LLVM::LLVMDialect::getStructRetAttrName())) {
      resultBuf = emitAllocaOne(sretTy.getValue(), location);
      operands.insert(operands.begin(), resultBuf);
    }

  // A struct argument goes in its register form or, if byval, as a pointer
  // to a copy. Both reach the declared type through a stack slot.
  auto fnTy = llvmFunc.getFunctionType();
  for (unsigned i = 0; i < fnTy.getNumParams() && i < operands.size(); i++) {
    if (!mlir::isa<mlir::LLVM::LLVMStructType>(operands[i].getType()))
      continue;
    auto byvalTy = llvmFunc.getArgAttrOfType<mlir::TypeAttr>(
        i, mlir::LLVM::LLVMDialect::getByValAttrName());
    auto slotTy = byvalTy ? byvalTy.getValue() : fnTy.getParamType(i);
    if (operands[i].getType() == slotTy && !byvalTy)
      continue;
    auto slot = emitAllocaOne(slotTy, location);
    mlir::LLVM::StoreOp::create(builder, location, operands[i], slot);
    operands[i] =
        byvalTy ? slot
                : mlir::LLVM::LoadOp::create(builder, location, slotTy, slot)
                      .getResult();
  }

  // C's default argument promotions for variadic args: float to double,
  // and char (signed) and bool (unsigned) to int.
  if (fnTy.isVarArg()) {
    unsigned fixedCount = fnTy.getNumParams();
    for (unsigned i = fixedCount; i < operands.size(); i++) {
      auto type = operands[i].getType();
      if (type.isF32())
        operands[i] = mlir::LLVM::FPExtOp::create(
            builder, location, builder.getF64Type(), operands[i]);
      else if (type.isInteger(8))
        operands[i] = mlir::LLVM::SExtOp::create(
            builder, location, builder.getI32Type(), operands[i]);
      else if (type.isInteger(1))
        operands[i] = mlir::LLVM::ZExtOp::create(
            builder, location, builder.getI32Type(), operands[i]);
    }
  }
  // The declaration's types, which differ from sammine's for a struct.
  auto llvmCallOp =
      mlir::LLVM::CallOp::create(builder, location, llvmFunc, operands);
  // The call needs the declaration's sret and byval too: LLVM's optimizer
  // reads them from the call, and without them drops the stores that fill
  // a byval copy.
  if (auto declArgAttrs = llvmFunc.getArgAttrsAttr()) {
    llvm::SmallVector<mlir::Attribute> callArgAttrs(declArgAttrs.begin(),
                                                    declArgAttrs.end());
    callArgAttrs.resize(operands.size(), builder.getDictionaryAttr({}));
    llvmCallOp.setArgAttrsAttr(builder.getArrayAttr(callArgAttrs));
  }
  if (!resultBuf && llvmCallOp.getNumResults() == 0)
    return nullptr;
  auto semTy = convertType(semRetType);
  if (resultBuf)
    return mlir::LLVM::LoadOp::create(builder, location, semTy, resultBuf);
  // A struct returned in registers comes back as its register type; it
  // reaches the struct type through memory.
  auto result = llvmCallOp.getResult();
  if (result.getType() == semTy)
    return result;
  auto slot = emitAllocaOne(result.getType(), location);
  mlir::LLVM::StoreOp::create(builder, location, result, slot);
  return mlir::LLVM::LoadOp::create(builder, location, semTy, slot);
}

mlir::Value
MLIRGenImpl::emitDirectCall(AST::CallExprAST *ast, const std::string &callee,
                            llvm::SmallVector<mlir::Value, 4> &operands,
                            mlir::Location location) {
  // sret: if call returns an array, allocate local buffer, pass as extra arg
  if (ast->get_type().type_kind == TypeKind::Array) {
    auto arrMlirType = convertType(ast->get_type());
    auto localBuf = emitAllocaOne(arrMlirType, location);

    // GPU wrapper takes memref sret — convert ptr to memref at call site.
    if (targetGPU() && theModule.lookupSymbol<mlir::func::FuncOp>(callee) &&
        theModule.lookupSymbol<mlir::func::FuncOp>(callee)->hasAttr(
            "sammine.kernel_wrapper")) {
      auto retType = ast->get_type();
      auto arrType = std::get<ArrayType>(retType.type_data);
      auto arrSize = static_cast<int64_t>(arrType.get_size());
      auto elemMLIRType = convertTypeForKernel(arrType.get_element());
      auto sretMemref =
          buildMemrefFromPtr(localBuf, arrSize, elemMLIRType, location);
      operands.push_back(sretMemref);
    } else {
      operands.push_back(localBuf);
    }

    emitSammineCall(callee, operands, {}, location);
    return localBuf;
  }

  llvm::SmallVector<mlir::Type, 1> resultTypes;
  if (ast->get_type().type_kind != TypeKind::Unit)
    resultTypes.push_back(convertType(ast->get_type()));

  // A C function goes through llvm.call, with C's way of moving structs.
  if (auto llvmFunc = theModule.lookupSymbol<mlir::LLVM::LLVMFuncOp>(callee))
    return emitCCall(llvmFunc, operands, ast->get_type(), location);

  auto results = emitSammineCall(callee, operands, resultTypes, location);
  return results.empty() ? nullptr : results.front();
}

mlir::Value MLIRGenImpl::emitBuiltinInstanceCall(AST::CallExprAST *ast,
                                                 const MonomorphizedKey &key) {
  llvm::SmallVector<mlir::Value, 2> operands;
  for (auto &arg : ast->arguments) {
    auto val = emitRValue(arg.get());
    if (!val)
      return nullptr;
    operands.push_back(val);
  }

  auto location = loc(ast);
  if (key.name == kConvertClass)
    return emitBuiltinConvert(operands[0], key.type_args[0], key.type_args[1],
                              location);
  if (auto op = operator_of_class(key.name)) {
    auto &type = key.type_args[0];
    if (auto cmp =
            emitComparison(*op, operands[0], operands[1], type, location))
      return cmp;
    if (isFloatType(type))
      return emitFloatArithOp(builder, location, operands[0], operands[1], *op);
    return emitIntArithOp(builder, location, operands[0], operands[1], *op,
                          isUnsignedIntegerType(type));
  }
  imm_error(fmt::format("no code for built-in instance {}<{}>", key.name,
                        key.type_args_string()),
            ast->get_location());
}

mlir::Value MLIRGenImpl::emitCallExpr(AST::CallExprAST *ast) {
  // Enum constructor — handled separately
  auto *cp = props_.call(ast->id());
  if (cp && cp->is_enum_constructor)
    return emitEnumConstructor(ast);
  // An instance the compiler provides has no function to call.
  if (cp && cp->builtin_instance)
    return emitBuiltinInstanceCall(ast, *cp->builtin_instance);

  auto location = loc(ast);

  // Resolve callee name first — needed to detect kernel wrappers. Name it as
  // its definition is named, so a call inside a library module reaches the
  // module-qualified function.
  std::string callee = mangleName(cp && cp->resolved_name.has_value()
                                      ? *cp->resolved_name
                                      : ast->functionName);

  if (auto it = c_symbols.find(callee); it != c_symbols.end())
    callee = it->second;

  // Check if callee is a kernel wrapper (takes arrays by ptr, not by value)
  bool isKernelWrapper = false;
  if (auto funcOp = theModule.lookupSymbol<mlir::func::FuncOp>(callee))
    isKernelWrapper = funcOp->hasAttr("sammine.kernel_wrapper");

  // Emit arguments — kernel wrappers take array args as !llvm.ptr (no copy),
  // normal functions take arrays by value via emitRValue (loads from alloca).
  llvm::SmallVector<mlir::Value, 4> operands;
  for (auto &arg : ast->arguments) {
    if (isKernelWrapper && arg->get_type().type_kind == TypeKind::Array) {
      auto val = emitExpr(arg.get()); // returns !llvm.ptr directly
      if (val) {
        if (targetGPU()) {
          // GPU wrapper takes memref args — convert ptr to memref at call site.
          auto argType = arg->get_type();
          auto arrType = std::get<ArrayType>(argType.type_data);
          auto arrSize = static_cast<int64_t>(arrType.get_size());
          auto elemMLIRType = convertTypeForKernel(arrType.get_element());
          val = buildMemrefFromPtr(val, arrSize, elemMLIRType, location);
        }
        operands.push_back(val);
      }
    } else {
      auto val = emitRValue(arg.get());
      if (val)
        operands.push_back(val);
    }
  }

  if (!theModule.lookupSymbol(callee))
    return emitIndirectCall(ast, operands);

  if (cp && cp->is_partial)
    return emitPartialApplication(ast, callee, operands);

  return emitDirectCall(ast, callee, operands, location);
}

mlir::Value MLIRGenImpl::emitReturnStmt(AST::ReturnStmtAST *ast) {
  auto location = loc(ast);

  if (ast->return_expr) {
    auto val = emitExpr(ast->return_expr.get());
    if (currentSretBuffer && val) {
      // sret: load whole array from source, store into output buffer
      auto arrMlirType = convertType(ast->return_expr->get_type());
      auto arrVal =
          mlir::LLVM::LoadOp::create(builder, location, arrMlirType, val);
      mlir::LLVM::StoreOp::create(builder, location, arrVal, currentSretBuffer);
      mlir::func::ReturnOp::create(builder, location);
    } else if (val) {
      mlir::func::ReturnOp::create(builder, location, mlir::ValueRange{val});
    } else {
      mlir::func::ReturnOp::create(builder, location);
    }
  } else {
    mlir::func::ReturnOp::create(builder, location);
  }

  return nullptr;
}

} // namespace sammine_lang
