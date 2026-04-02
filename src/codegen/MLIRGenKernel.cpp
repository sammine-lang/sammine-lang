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

#include "codegen/MLIRGenImpl.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinTypes.h"

#include "fmt/core.h"

namespace sammine_lang {

// ===--- Entry: kernel definition ---===

void MLIRGenImpl::emitKernelDef(AST::KernelDefAST *kd) {
  auto location = loc(kd);
  auto publicName = mangleName(kd->Prototype->functionName);
  auto internalName = kKernelPrefix.str() + publicName;

  // === 1. Emit the internal kernel function (tensor types) ===
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

    // DPS: for array-returning kernels, the last entry block argument is the
    // output tensor provided by the caller (the wrapper).
    auto protoFT = std::get<FunctionType>(kd->Prototype->get_type().type_data);
    auto retSamType = protoFT.get_return_type();
    mlir::Value dpsOutput;
    if (retSamType.type_kind == TypeKind::Array) {
      dpsOutput = entryBlock.getArgument(entryBlock.getNumArguments() - 1);
    }

    // Dispatch to the appropriate kernel expression handler
    if (kd->Body->expressions.empty()) {
      mlir::func::ReturnOp::create(builder, location);
    } else {
      auto *firstExpr = kd->Body->expressions[0].get();

      if (auto *numExpr = llvm::dyn_cast<AST::KernelNumberExprAST>(firstExpr)) {
        auto retType = kernelFuncType.getResults()[0];
        if (mlir::isa<mlir::FloatType>(retType)) {
          double val = std::stod(numExpr->number);
          auto constVal =
              mlir::arith::ConstantFloatOp::create(
                  builder, location, mlir::cast<mlir::FloatType>(retType),
                  llvm::APFloat(val))
                  .getResult();
          mlir::func::ReturnOp::create(builder, location,
                                       mlir::ValueRange{constVal});
        } else {
          int64_t val = std::stoll(numExpr->number);
          auto constVal = mlir::arith::ConstantIntOp::create(builder, location,
                                                             retType, val)
                              .getResult();
          mlir::func::ReturnOp::create(builder, location,
                                       mlir::ValueRange{constVal});
        }
      } else if (auto *mapExpr =
                     llvm::dyn_cast<AST::KernelMapExprAST>(firstExpr)) {
        emitKernelMapExpr(mapExpr, entryBlock, kd, location, dpsOutput);
      } else if (auto *reduceExpr =
                     llvm::dyn_cast<AST::KernelReduceExprAST>(firstExpr)) {
        emitKernelReduceExpr(reduceExpr, entryBlock, kd, location);
      } else {
        imm_error("Unknown kernel expression type", kd->get_location());
      }
    }
  } // end symbol table scope

  // === 2. Emit the public CPU-ABI wrapper ===
  emitKernelWrapper(kd, internalName, publicName, location);
}

// ===--- Kernel map ---===

void MLIRGenImpl::emitKernelMapExpr(AST::KernelMapExprAST *mapExpr,
                                    mlir::Block &entryBlock,
                                    AST::KernelDefAST *kd,
                                    mlir::Location location,
                                    mlir::Value dpsOutput) {
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

  assert(dpsOutput && "Map kernel must have DPS output parameter");

  auto mapOp = mlir::linalg::MapOp::create(
      builder, location,
      /*inputs=*/mlir::ValueRange{inputTensor},
      /*init=*/dpsOutput,
      /*bodyBuilder=*/
      [&](mlir::OpBuilder &b, mlir::Location loc, mlir::ValueRange args) {
        auto lambdaParamName = mapExpr->lambda_proto->parameterVectors[0]->name;

        mlir::OpBuilder::InsertionGuard builderGuard(builder);
        builder.setInsertionPointToStart(b.getInsertionBlock());

        decltype(symbolTable)::Guard lambdaScope(symbolTable);
        symbolTable.registerNameT(lambdaParamName, args[0]);

        bool savedKernelCtx = in_kernel_lambda_body;
        in_kernel_lambda_body = true;

        mlir::Value bodyResult = nullptr;
        for (auto &expr : mapExpr->lambda_body->Statements) {
          bodyResult = emitExpr(expr.get());
        }

        in_kernel_lambda_body = savedKernelCtx;

        assert(bodyResult && "Lambda body must produce a value");
        mlir::linalg::YieldOp::create(b, loc, mlir::ValueRange{bodyResult});
      });

  mlir::func::ReturnOp::create(builder, location, mapOp->getResults());
}

// ===--- Kernel reduce ---===

void MLIRGenImpl::emitKernelReduceExpr(AST::KernelReduceExprAST *reduceExpr,
                                       mlir::Block &entryBlock,
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

  auto identityVal = emitExpr(reduceExpr->identity.get());
  assert(identityVal && "Identity expression must produce a value");

  auto emptyScalar = mlir::tensor::EmptyOp::create(
      builder, location, llvm::ArrayRef<int64_t>{}, elemMLIRType);

  auto fillOp = mlir::linalg::FillOp::create(
      builder, location,
      /*inputs=*/mlir::ValueRange{identityVal},
      /*outputs=*/mlir::ValueRange{emptyScalar.getResult()});
  auto filledInit = fillOp.getResultTensors()[0];

  auto reduceOp = mlir::linalg::ReduceOp::create(
      builder, location,
      /*inputs=*/mlir::ValueRange{inputTensor},
      /*inits=*/mlir::ValueRange{filledInit},
      /*dimensions=*/llvm::ArrayRef<int64_t>{0},
      /*bodyBuilder=*/
      [&](mlir::OpBuilder &b, mlir::Location loc, mlir::ValueRange args) {
        mlir::OpBuilder::InsertionGuard builderGuard(builder);
        builder.setInsertionPointToStart(b.getInsertionBlock());

        bool savedKernelCtx = in_kernel_lambda_body;
        in_kernel_lambda_body = true;

        mlir::Value result;
        auto opTok = reduceExpr->op_tok->tok_type;

        if (isFloatType(elemSamType)) {
          switch (opTok) {
          case TokenType::TokADD:
            result = mlir::arith::AddFOp::create(b, loc, args[0], args[1])
                         .getResult();
            break;
          case TokenType::TokSUB:
            result = mlir::arith::SubFOp::create(b, loc, args[0], args[1])
                         .getResult();
            break;
          case TokenType::TokMUL:
            result = mlir::arith::MulFOp::create(b, loc, args[0], args[1])
                         .getResult();
            break;
          case TokenType::TokDIV:
            result = mlir::arith::DivFOp::create(b, loc, args[0], args[1])
                         .getResult();
            break;
          default:
            imm_error("Unsupported reduce operator");
          }
        } else {
          bool isUnsigned = isUnsignedIntegerType(elemSamType);
          switch (opTok) {
          case TokenType::TokADD:
            result = mlir::arith::AddIOp::create(b, loc, args[0], args[1])
                         .getResult();
            break;
          case TokenType::TokSUB:
            result = mlir::arith::SubIOp::create(b, loc, args[0], args[1])
                         .getResult();
            break;
          case TokenType::TokMUL:
            result = mlir::arith::MulIOp::create(b, loc, args[0], args[1])
                         .getResult();
            break;
          case TokenType::TokDIV:
            if (isUnsigned)
              result = mlir::arith::DivUIOp::create(b, loc, args[0], args[1])
                           .getResult();
            else
              result = mlir::arith::DivSIOp::create(b, loc, args[0], args[1])
                           .getResult();
            break;
          default:
            imm_error("Unsupported reduce operator");
          }
        }

        in_kernel_lambda_body = savedKernelCtx;

        mlir::linalg::YieldOp::create(b, loc, mlir::ValueRange{result});
      });

  auto scalarResult = mlir::tensor::ExtractOp::create(
      builder, location, reduceOp->getResult(0), mlir::ValueRange{});

  mlir::func::ReturnOp::create(builder, location,
                               mlir::ValueRange{scalarResult});
}

// ====== Kernel wrapper (host<->device boundary) ======

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

  // === Marshal inputs ===
  llvm::SmallVector<mlir::Value> kernelArgs;
  llvm::SmallVector<mlir::Value> deviceMemrefs; // GPU only: for dealloc
  size_t blockArgIdx = 0;

  for (size_t i = 0; i < kd->Prototype->parameterVectors.size(); ++i) {
    auto paramType = kd->Prototype->parameterVectors[i]->get_type();
    auto blockArg =
        entryBlock.getArgument(static_cast<unsigned>(blockArgIdx++));

    if (paramType.type_kind == TypeKind::Array) {
      if (targetGPU()) {
        auto deviceMem = gpuCopyToDevice(blockArg, location);
        kernelArgs.push_back(deviceMem);
        deviceMemrefs.push_back(deviceMem);
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

  // === DPS output ===
  mlir::Value hostOutMemref, deviceOut;
  if (returnsArray) {
    auto sretArg = entryBlock.getArgument(entryBlock.getNumArguments() - 1);
    if (targetGPU()) {
      hostOutMemref = sretArg;
      deviceOut = gpuCopyToDevice(hostOutMemref, location);
      kernelArgs.push_back(deviceOut);
    } else {
      auto &retArrType = std::get<ArrayType>(retSamType.type_data);
      auto arrSize = static_cast<int64_t>(retArrType.get_size());
      auto elemMLIRType = convertTypeForKernel(retArrType.get_element());
      hostOutMemref =
          buildMemrefFromPtr(sretArg, arrSize, elemMLIRType, location);
      kernelArgs.push_back(hostOutMemref);
    }
  }

  // === Call kernel ===
  auto memrefFuncType =
      buildKernelFuncType(kd->Prototype.get(), /*asMemref=*/true);
  auto callOp = mlir::func::CallOp::create(
      builder, location, internalName, memrefFuncType.getResults(), kernelArgs);

  // === GPU epilogue ===
  if (targetGPU()) {
    if (returnsArray)
      gpuCopyToHostAndDealloc(deviceOut, hostOutMemref, location);
    for (auto dm : deviceMemrefs)
      gpuDealloc(dm, location);
  }

  // === Return ===
  if (returnsArray || retSamType.type_kind == TypeKind::Unit) {
    mlir::func::ReturnOp::create(builder, location);
  } else {
    mlir::func::ReturnOp::create(builder, location, callOp.getResults());
  }
}

// ===--- GPU memory helpers ---===

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

// ===--- Kernel type helpers ---===

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
      if (retType.type_kind == TypeKind::Array)
        argTypes.push_back(mlirRetType);
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
