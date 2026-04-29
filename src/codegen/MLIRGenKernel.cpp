// Kernel codegen: kernel definition, map, reduce, wrapper, GPU marshalling.
//
// Call graph:
//   emitKernelDef              ← entry (called from emitDefinition in MLIRGen.cpp)
//   ├── emitKernelMapExpr
//   ├── emitKernelReduceExpr
//   └── emitKernelWrapper
//       ├── gpuCopyToDevice
//       ├── gpuCopyToHostAndDealloc
//       └── gpuDealloc
//   buildGpuWrapperFuncType    ← called from generate() in MLIRGen.cpp
//   buildKernelFuncType        ← called from generate() + emitKernelDef + emitKernelWrapper
//   convertTypeForKernel       ← leaf, used by all type builders

#include "codegen/MLIRGenBinaryOps.h"
#include "codegen/MLIRGenImpl.h"
#include "codegen/SammineDialect.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinTypes.h"
#include "stablehlo/dialect/StablehloOps.h"

#include "fmt/core.h"

namespace sammine_lang {

// --- Entry: kernel definition ---

void MLIRGenImpl::emitKernelDef(AST::KernelDefAST *kd) {
  auto location = loc(kd);
  auto publicName = mangleName(kd->Prototype->functionName);
  auto internalName = kKernelPrefix.str() + publicName;

  //  1. Emit the internal kernel function (tensor types) 
  {
    decltype(symbolTable)::Guard scope(symbolTable);

    auto kernelFuncType = buildKernelFuncType(kd->Prototype.get());
    auto funcOp = kernelModule.lookupSymbol<mlir::func::FuncOp>(internalName);
    assert(
        funcOp &&
        "Internal kernel function should be forward-declared in kernel module");

    auto &entryBlock = *funcOp.addEntryBlock();
    builder.setInsertionPointToStart(&entryBlock);

    // Bind parameters as tensor SSA values (no alloca wrapper).
    for (size_t i = 0; i < kd->Prototype->parameterVectors.size(); ++i) {
      auto &param = kd->Prototype->parameterVectors[i];
      auto argVal = entryBlock.getArgument(static_cast<unsigned>(i));
      symbolTable.registerNameT(param->name, argVal);
    }

    // StableHLO ops are functional (tensor in → tensor out), so the inner
    // kernel function no longer needs a DPS output parameter.
    mlir::Value dpsOutput; // unused, kept for API compat with emitKernelMapExpr

    // Each handler produces a result value; the single func.return that
    // terminates the function is emitted here, not inside the handlers.
    // Empty body → void return. Unknown expression → imm_error leaves
    // resultVal null, and no terminator is emitted (matches prior behavior).
    if (kd->Body->expressions.empty()) {
      mlir::func::ReturnOp::create(builder, location);
    } else {
      auto *firstExpr = kd->Body->expressions[0].get();
      mlir::Value resultVal;

      if (auto *numExpr = llvm::dyn_cast<AST::KernelNumberExprAST>(firstExpr)) {
        auto retType = kernelFuncType.getResults()[0];
        if (mlir::isa<mlir::FloatType>(retType)) {
          double val = std::stod(numExpr->number);
          resultVal = mlir::arith::ConstantFloatOp::create(
                          builder, location,
                          mlir::cast<mlir::FloatType>(retType),
                          llvm::APFloat(val))
                          .getResult();
        } else {
          int64_t val = std::stoll(numExpr->number);
          resultVal = mlir::arith::ConstantIntOp::create(builder, location,
                                                         retType, val)
                          .getResult();
        }
      } else if (auto *mapExpr =
                     llvm::dyn_cast<AST::KernelMapExprAST>(firstExpr)) {
        resultVal = emitKernelMapExpr(mapExpr, kd, location, dpsOutput);
      } else if (auto *reduceExpr =
                     llvm::dyn_cast<AST::KernelReduceExprAST>(firstExpr)) {
        resultVal = emitKernelReduceExpr(reduceExpr, kd, location);
      } else {
        imm_error("Unknown kernel expression type", kd->get_location());
      }

      if (resultVal)
        mlir::func::ReturnOp::create(builder, location,
                                     mlir::ValueRange{resultVal});
    }
  } // end symbol table scope

  //  2. Emit the public CPU-ABI wrapper 
  emitKernelWrapper(kd, internalName, publicName, location);
}

// --- Kernel map ---

mlir::Value MLIRGenImpl::emitKernelMapExpr(AST::KernelMapExprAST *mapExpr,
                                           AST::KernelDefAST *kd,
                                           mlir::Location location,
                                           mlir::Value /*dpsOutput*/) {
  auto inputTensor = symbolTable.get_from_name(mapExpr->input_name);

  Type foundInputType = Type::NonExistent();
  for (size_t i = 0; i < kd->Prototype->parameterVectors.size(); ++i) {
    if (kd->Prototype->parameterVectors[i]->name == mapExpr->input_name) {
      foundInputType = kd->Prototype->parameterVectors[i]->get_type();
      break;
    }
  }
  assert(foundInputType.type_kind != TypeKind::NonExistent &&
         "Map input must be a kernel parameter");

  auto inputRankedType =
      mlir::cast<mlir::RankedTensorType>(inputTensor.getType());
  auto resultType = inputRankedType;

  mlir::stablehlo::MapOp mapOp;

  {
    // Guard the insertion point so we return to the function body after
    // building the computation region (createBlock moves the IP).
    mlir::OpBuilder::InsertionGuard guard(builder);

    mapOp = mlir::stablehlo::MapOp::create(
        builder, location,
        /*resultType0=*/resultType,
        /*inputs=*/mlir::ValueRange{inputTensor},
        /*dimensions=*/llvm::ArrayRef<int64_t>{0});

    // Build the computation region. stablehlo.map body takes 0-d tensor args.
    auto elemType = inputRankedType.getElementType();
    auto scalarTensorType = mlir::RankedTensorType::get({}, elemType);
    auto &computation = mapOp.getComputation();
    auto *entryBlock = builder.createBlock(
        &computation, {}, {scalarTensorType}, {location});
    builder.setInsertionPointToStart(entryBlock);

    auto lambdaParamName = mapExpr->lambda_proto->parameterVectors[0]->name;
    auto arg0 = entryBlock->getArgument(0);

    // Extract scalar from 0-d tensor, run the lambda body, wrap back.
    auto scalar = mlir::tensor::ExtractOp::create(
        builder, location, arg0, mlir::ValueRange{});

    decltype(symbolTable)::Guard lambdaScope(symbolTable);
    symbolTable.registerNameT(lambdaParamName, scalar.getResult());

    bool savedKernelCtx = in_kernel_lambda_body;
    in_kernel_lambda_body = true;

    mlir::Value bodyResult = nullptr;
    for (auto &expr : mapExpr->lambda_body->Statements) {
      bodyResult = emitExpr(expr.get());
    }

    in_kernel_lambda_body = savedKernelCtx;

    assert(bodyResult && "Lambda body must produce a value");

    // Wrap scalar result back to 0-d tensor for stablehlo.return.
    auto emptyTensor = mlir::tensor::EmptyOp::create(
        builder, location, llvm::ArrayRef<int64_t>{}, bodyResult.getType());
    auto resultTensor = mlir::tensor::InsertOp::create(
        builder, location, bodyResult, emptyTensor, mlir::ValueRange{});
    mlir::stablehlo::ReturnOp::create(builder, location,
                                      mlir::ValueRange{resultTensor});
  }

  return mapOp->getResult(0);
}

// --- Kernel reduce ---

mlir::Value
MLIRGenImpl::emitKernelReduceExpr(AST::KernelReduceExprAST *reduceExpr,
                                  AST::KernelDefAST *kd,
                                  mlir::Location location) {
  auto inputTensor = symbolTable.get_from_name(reduceExpr->input_name);

  Type foundInputType = Type::NonExistent();
  for (size_t i = 0; i < kd->Prototype->parameterVectors.size(); ++i) {
    if (kd->Prototype->parameterVectors[i]->name == reduceExpr->input_name) {
      foundInputType = kd->Prototype->parameterVectors[i]->get_type();
      break;
    }
  }
  assert(foundInputType.type_kind != TypeKind::NonExistent &&
         "Reduce input must be a kernel parameter");
  auto arrType = std::get<ArrayType>(foundInputType.type_data);
  auto elemSamType = arrType.get_element();
  auto elemMLIRType = convertTypeForKernel(elemSamType);

  // Build the init value as a 0-d tensor for stablehlo.reduce.
  auto identityVal = emitExpr(reduceExpr->identity.get());
  assert(identityVal && "Identity expression must produce a value");
  auto emptyInit = mlir::tensor::EmptyOp::create(
      builder, location, llvm::ArrayRef<int64_t>{}, elemMLIRType);
  auto initTensor = mlir::tensor::InsertOp::create(
      builder, location, identityVal, emptyInit, mlir::ValueRange{});

  // Dimension to reduce along (future-proofed, default 0 for 1-D arrays).
  int64_t reduceDim = 0;

  auto scalarTensorType = mlir::RankedTensorType::get({}, elemMLIRType);

  mlir::stablehlo::ReduceOp reduceOp;

  {
    // Guard the insertion point so we return to the function body after
    // building the reducer region (createBlock moves the IP).
    mlir::OpBuilder::InsertionGuard guard(builder);

    reduceOp = mlir::stablehlo::ReduceOp::create(
        builder, location,
        /*inputs=*/mlir::ValueRange{inputTensor},
        /*init_values=*/mlir::ValueRange{initTensor},
        /*dimensions=*/llvm::ArrayRef<int64_t>{reduceDim});

    // Build the reducer body. stablehlo.reduce body takes 0-d tensor pairs.
    auto &body = reduceOp.getBody();
    auto *entryBlock = builder.createBlock(
        &body, {}, {scalarTensorType, scalarTensorType}, {location, location});
    builder.setInsertionPointToStart(entryBlock);

    auto lhsTensor = entryBlock->getArgument(0);
    auto rhsTensor = entryBlock->getArgument(1);

    // Extract scalars, apply binary op, wrap back to 0-d tensor.
    auto lhs = mlir::tensor::ExtractOp::create(
        builder, location, lhsTensor, mlir::ValueRange{});
    auto rhs = mlir::tensor::ExtractOp::create(
        builder, location, rhsTensor, mlir::ValueRange{});

    auto opTok = reduceExpr->op_tok->tok_type;
    mlir::Value result =
        isFloatType(elemSamType)
            ? emitFloatArithOp(builder, location, lhs, rhs, opTok)
            : emitIntArithOp(builder, location, lhs, rhs, opTok,
                             isUnsignedIntegerType(elemSamType));
    if (!result)
      imm_error("Unsupported reduce operator");

    auto emptyResult = mlir::tensor::EmptyOp::create(
        builder, location, llvm::ArrayRef<int64_t>{}, result.getType());
    auto resultTensor = mlir::tensor::InsertOp::create(
        builder, location, result, emptyResult, mlir::ValueRange{});
    mlir::stablehlo::ReturnOp::create(builder, location,
                                      mlir::ValueRange{resultTensor});
  }

  // stablehlo.reduce returns a 0-d tensor; extract the scalar.
  auto scalarResult = mlir::tensor::ExtractOp::create(
      builder, location, reduceOp->getResult(0), mlir::ValueRange{});

  return scalarResult.getResult();
}

//  Kernel wrapper (host<->device boundary) 

void MLIRGenImpl::emitKernelWrapper(AST::KernelDefAST *kd,
                                    const std::string &internalName,
                                    const std::string &publicName,
                                    mlir::Location location) {
  auto wrapperOp = theModule.lookupSymbol<mlir::func::FuncOp>(publicName);
  assert(wrapperOp && "Wrapper function should be forward-declared");

  mlir::OpBuilder::InsertionGuard guard(builder);

  auto &entryBlock = *wrapperOp.addEntryBlock();
  builder.setInsertionPointToStart(&entryBlock);

  auto protoFT = std::get<FunctionType>(kd->Prototype->get_type().type_data);
  auto retSamType = protoFT.get_return_type();
  bool returnsArray = retSamType.type_kind == TypeKind::Array;

  //  Marshal inputs 
  llvm::SmallVector<mlir::Value> kernelArgs;
  llvm::SmallVector<mlir::Value> deviceMemrefs; // GPU only: for dealloc
  size_t blockArgIdx = 0;

  for (size_t i = 0; i < kd->Prototype->parameterVectors.size(); ++i) {
    auto paramType = kd->Prototype->parameterVectors[i]->get_type();
    auto blockArg =
        entryBlock.getArgument(static_cast<unsigned>(blockArgIdx++));

    if (paramType.type_kind == TypeKind::Array) {
      if (targetGPU()) {
        auto deviceMem = smn::ToDeviceOp::create(builder, location, blockArg.getType(), blockArg);
        kernelArgs.push_back(deviceMem.getResult());
        deviceMemrefs.push_back(deviceMem.getResult());
      } else {
        auto &arrType = std::get<ArrayType>(paramType.type_data);
        auto arrSize = static_cast<int64_t>(arrType.get_size());
        auto elemMLIRType = convertTypeForKernel(arrType.get_element());
        auto hostMemref =
            buildMemrefFromPtr(blockArg, arrSize, elemMLIRType, location);
        kernelArgs.push_back(hostMemref);
      }
    } else {
      kernelArgs.push_back(blockArg);
    }
  }

  //  Prepare output buffer (for array-returning kernels)
  mlir::Value hostOutMemref;
  if (returnsArray) {
    auto sretArg = entryBlock.getArgument(entryBlock.getNumArguments() - 1);
    auto &retArrType = std::get<ArrayType>(retSamType.type_data);
    auto arrSize = static_cast<int64_t>(retArrType.get_size());
    auto elemMLIRType = convertTypeForKernel(retArrType.get_element());
    if (targetGPU()) {
      hostOutMemref = sretArg;
    } else {
      hostOutMemref =
          buildMemrefFromPtr(sretArg, arrSize, elemMLIRType, location);
    }
  }

  //  Call kernel (no DPS — kernel returns result functionally)
  auto memrefFuncType =
      buildKernelFuncType(kd->Prototype.get(), /*asMemref=*/true);
  auto callOp = mlir::func::CallOp::create(
      builder, location, internalName, memrefFuncType.getResults(), kernelArgs);

  //  Copy result to output buffer for array returns
  if (returnsArray) {
    auto resultMemref = callOp.getResult(0);
    if (targetGPU()) {
      smn::ToHostOp::create(builder, location, hostOutMemref.getType(),
                            resultMemref);
    } else {
      mlir::memref::CopyOp::create(builder, location, resultMemref,
                                   hostOutMemref);
    }
  }

  //  GPU epilogue: dealloc device input copies
  if (targetGPU()) {
    for (auto dm : deviceMemrefs)
      gpuDealloc(dm, location);
  }

  //  Return
  if (returnsArray || retSamType.type_kind == TypeKind::Unit) {
    mlir::func::ReturnOp::create(builder, location);
  } else {
    mlir::func::ReturnOp::create(builder, location, callOp.getResults());
  }
}

// --- GPU memory helpers ---

mlir::Value MLIRGenImpl::gpuCopyToDevice(mlir::Value hostMemref,
                                         mlir::Location loc) {
  auto memrefType = llvm::cast<mlir::MemRefType>(hostMemref.getType());
  auto tokenType = mlir::gpu::AsyncTokenType::get(builder.getContext());
  auto waitOp = mlir::gpu::WaitOp::create(builder, loc, tokenType,
                                           /*asyncDependencies=*/mlir::ValueRange{});
  auto allocOp = mlir::gpu::AllocOp::create(
      builder, loc, memrefType, tokenType,
      /*asyncDependencies=*/mlir::ValueRange{waitOp.getAsyncToken()},
      /*dynamicSizes=*/mlir::ValueRange{},
      /*symbolOperands=*/mlir::ValueRange{});
  auto memcpyOp = mlir::gpu::MemcpyOp::create(
      builder, loc, tokenType,
      /*asyncDependencies=*/mlir::ValueRange{allocOp.getAsyncToken()},
      allocOp.getMemref(), hostMemref);
  mlir::gpu::WaitOp::create(builder, loc, /*asyncToken=*/mlir::Type{},
                             /*asyncDependencies=*/mlir::ValueRange{memcpyOp.getAsyncToken()});
  return allocOp.getMemref();
}

void MLIRGenImpl::gpuCopyToHostAndDealloc(mlir::Value deviceMemref,
                                          mlir::Value hostMemref,
                                          mlir::Location loc) {
  auto tokenType = mlir::gpu::AsyncTokenType::get(builder.getContext());
  auto waitOp = mlir::gpu::WaitOp::create(builder, loc, tokenType,
                                           /*asyncDependencies=*/mlir::ValueRange{});
  auto memcpyOp = mlir::gpu::MemcpyOp::create(
      builder, loc, tokenType,
      /*asyncDependencies=*/mlir::ValueRange{waitOp.getAsyncToken()},
      hostMemref, deviceMemref);
  auto deallocOp = mlir::gpu::DeallocOp::create(
      builder, loc, tokenType,
      /*asyncDependencies=*/mlir::ValueRange{memcpyOp.getAsyncToken()},
      deviceMemref);
  mlir::gpu::WaitOp::create(builder, loc, /*asyncToken=*/mlir::Type{},
                             /*asyncDependencies=*/mlir::ValueRange{deallocOp.getAsyncToken()});
}

void MLIRGenImpl::gpuDealloc(mlir::Value deviceMemref, mlir::Location loc) {
  auto tokenType = mlir::gpu::AsyncTokenType::get(builder.getContext());
  auto waitOp = mlir::gpu::WaitOp::create(builder, loc, tokenType,
                                           /*asyncDependencies=*/mlir::ValueRange{});
  auto deallocOp = mlir::gpu::DeallocOp::create(
      builder, loc, tokenType,
      /*asyncDependencies=*/mlir::ValueRange{waitOp.getAsyncToken()},
      deviceMemref);
  mlir::gpu::WaitOp::create(builder, loc, /*asyncToken=*/mlir::Type{},
                             /*asyncDependencies=*/mlir::ValueRange{deallocOp.getAsyncToken()});
}

// --- Kernel type helpers ---

mlir::Type MLIRGenImpl::convertTypeForKernel(const Type &type, bool asMemref) {
  switch (type.type_kind) {
  case TypeKind::Array: {
    auto &arr = std::get<ArrayType>(type.type_data);
    auto elemSamType = arr.get_element();
    if (elemSamType.type_kind == TypeKind::Array)
      imm_error("Nested arrays not yet supported in kernel context");
    auto elemType = convertTypeForKernel(elemSamType, false);
    auto size = static_cast<int64_t>(arr.get_size());
    if (asMemref)
      return mlir::MemRefType::get({size}, elemType);
    return mlir::RankedTensorType::get({size}, elemType);
  }
  case TypeKind::I32_t:
  case TypeKind::Integer:
    return builder.getI32Type();
  case TypeKind::I64_t:
    return builder.getI64Type();
  case TypeKind::F64_t:
  case TypeKind::Flt:
    return builder.getF64Type();
  case TypeKind::F32_t:
    return builder.getF32Type();
  case TypeKind::U32_t:
    return builder.getI32Type();
  case TypeKind::U64_t:
    return builder.getI64Type();
  case TypeKind::Bool:
    return builder.getI1Type();
  case TypeKind::Char:
    return builder.getI8Type();
  case TypeKind::Unit:
    return mlir::NoneType::get(builder.getContext());
  case TypeKind::Pointer:
  case TypeKind::Function:
  case TypeKind::String:
  case TypeKind::Struct:
  case TypeKind::Enum:
  case TypeKind::Tuple:
    imm_error(fmt::format("Type '{}' is not valid in kernel context",
                          type.to_string()));
  default:
    imm_error("Unknown type kind in kernel context");
  }
}

mlir::FunctionType MLIRGenImpl::buildKernelFuncType(AST::PrototypeAST *proto,
                                                    bool asMemref) {
  llvm::SmallVector<mlir::Type, 4> argTypes;
  for (auto &param : proto->parameterVectors)
    argTypes.push_back(convertTypeForKernel(param->get_type(), asMemref));

  llvm::SmallVector<mlir::Type, 1> retTypes;
  if (proto->get_type().type_kind == TypeKind::Function) {
    auto ft = std::get<FunctionType>(proto->get_type().type_data);
    auto retType = ft.get_return_type();
    if (retType.type_kind != TypeKind::Unit) {
      auto mlirRetType = convertTypeForKernel(retType, asMemref);
      retTypes.push_back(mlirRetType);
    }
  }

  return builder.getFunctionType(argTypes, retTypes);
}

mlir::FunctionType
MLIRGenImpl::buildGpuWrapperFuncType(AST::PrototypeAST *proto) {
  llvm::SmallVector<mlir::Type, 4> argTypes;
  for (auto &param : proto->parameterVectors) {
    if (param->get_type().type_kind == TypeKind::Array)
      argTypes.push_back(
          convertTypeForKernel(param->get_type(), /*asMemref=*/true));
    else
      argTypes.push_back(convertType(param->get_type()));
  }
  llvm::SmallVector<mlir::Type, 1> retTypes;
  auto funcType = std::get<FunctionType>(proto->get_type().type_data);
  auto retType = funcType.get_return_type();
  if (retType.type_kind == TypeKind::Array) {
    argTypes.push_back(
        convertTypeForKernel(retType, /*asMemref=*/true));
  } else if (retType.type_kind != TypeKind::Unit) {
    retTypes.push_back(convertType(retType));
  }
  return builder.getFunctionType(argTypes, retTypes);
}

} // namespace sammine_lang
