// A kernel's code: its linalg function and its wrapper.
//
//   declare         ← before any function body is lowered
//   define          ← a kernel's definition
//   ├── defineMap
//   ├── defineReduce
//   └── defineWrapper
//       └── gpuDealloc

#include "codegen/KernelGen.h"
#include "cabi/KernelABI.h"
#include "codegen/ScalarOps.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinTypes.h"

#include "fmt/core.h"

namespace sammine_lang {

namespace {

constexpr llvm::StringLiteral kKernelPrefix = "__kernel_";

Type returnType(AST::PrototypeAST *proto) {
  return std::get<FunctionType>(proto->get_type().type_data).get_return_type();
}

bool isFloat(const Type &type) {
  return type.type_kind == TypeKind::F64_t ||
         type.type_kind == TypeKind::F32_t || type.type_kind == TypeKind::Flt;
}

bool isUnsigned(const Type &type) {
  return type.type_kind == TypeKind::U32_t || type.type_kind == TypeKind::U64_t;
}

} // namespace

void KernelGen::internalError(const std::string &msg) {
  sammine_util::abort(msg);
}

std::string KernelGen::publicName(AST::KernelDefAST *kd) const {
  return props_.symbol(kd->Prototype->functionName);
}

std::string KernelGen::internalName(AST::KernelDefAST *kd) const {
  return kKernelPrefix.str() + publicName(kd);
}

// --- Declarations ---

void KernelGen::declare(AST::KernelDefAST *kd) {
  if (kd->Prototype->is_generic())
    return;
  auto unknown = builder_.getUnknownLoc();
  auto internal = internalName(kd);
  auto declarePrivate = [&](mlir::ModuleOp module, mlir::FunctionType type) {
    if (module.lookupSymbol(internal))
      return;
    auto funcOp = mlir::func::FuncOp::create(unknown, internal, type);
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
    module.push_back(funcOp);
  };

  if (!kernelModule_)
    kernelModule_ = mlir::ModuleOp::create(unknown);
  // The kernel, in tensor types.
  declarePrivate(kernelModule_, kernelFunctionType(kd->Prototype.get()));
  // The kernel as its wrapper calls it, in memref types: replaced, when the
  // modules are merged, by its bufferized definition.
  declarePrivate(cpuModule_,
                 kernelFunctionType(kd->Prototype.get(), /*asMemref=*/true));

  auto name = publicName(kd);
  if (cpuModule_.lookupSymbol(name))
    return;
  auto wrapperType = wrapperFunctionType(kd->Prototype.get());
  auto funcOp = mlir::func::FuncOp::create(unknown, name, wrapperType);
  funcOp.setVisibility(library_ ? mlir::SymbolTable::Visibility::Public
                                : mlir::SymbolTable::Visibility::Private);
  funcOp->setAttr("sammine.kernel_wrapper", builder_.getUnitAttr());
  // An array it returns, it fills in a buffer passed last.
  if (returnType(kd->Prototype.get()).type_kind == TypeKind::Array)
    funcOp.setArgAttr(wrapperType.getNumInputs() - 1, smn::kResultBufferAttr,
                      builder_.getUnitAttr());
  cpuModule_.push_back(funcOp);
}

// --- The kernel ---

mlir::LogicalResult KernelGen::define(AST::KernelDefAST *kd) {
  if (kd->Prototype->is_generic())
    return mlir::success();
  auto location = loc(kd);
  auto kernelType = kernelFunctionType(kd->Prototype.get());
  auto funcOp =
      kernelModule_.lookupSymbol<mlir::func::FuncOp>(internalName(kd));
  assert(funcOp && "a kernel is declared before it is defined");

  mlir::OpBuilder::InsertionGuard guard(builder_);
  auto &entryBlock = *funcOp.addEntryBlock();
  builder_.setInsertionPointToStart(&entryBlock);

  params_.clear();
  for (auto [param, arg] :
       llvm::zip(kd->Prototype->parameterVectors, entryBlock.getArguments()))
    params_[param->name] = arg;

  // A kernel returning an array fills the tensor its caller passes last.
  mlir::Value output;
  if (returnType(kd->Prototype.get()).type_kind == TypeKind::Array)
    output = entryBlock.getArgument(entryBlock.getNumArguments() - 1);

  if (kd->Body->expressions.empty()) {
    mlir::func::ReturnOp::create(builder_, location);
  } else {
    auto *expr = kd->Body->expressions[0].get();
    mlir::FailureOr<mlir::Value> result;
    if (auto *num = llvm::dyn_cast<AST::KernelNumberExprAST>(expr)) {
      auto type = kernelType.getResults()[0];
      if (auto floatType = mlir::dyn_cast<mlir::FloatType>(type))
        result = mlir::arith::ConstantFloatOp::create(
                     builder_, location, floatType,
                     llvm::APFloat(std::stod(num->number)))
                     .getResult();
      else
        result = mlir::arith::ConstantIntOp::create(
                     builder_, location, type,
                     sammine_util::literal_int_bits(num->number))
                     .getResult();
    } else if (auto *map = llvm::dyn_cast<AST::KernelMapExprAST>(expr)) {
      result = defineMap(map, location, output);
    } else if (auto *reduce = llvm::dyn_cast<AST::KernelReduceExprAST>(expr)) {
      result = defineReduce(reduce, kd, location);
    } else {
      internalError("Unknown kernel expression type");
    }
    if (mlir::failed(result))
      return mlir::failure();
    mlir::func::ReturnOp::create(builder_, location, mlir::ValueRange{*result});
  }

  defineWrapper(kd, location);
  return mlir::success();
}

mlir::FailureOr<mlir::Value>
KernelGen::defineMap(AST::KernelMapExprAST *mapExpr, mlir::Location location,
                     mlir::Value output) {
  auto input = params_.lookup(mapExpr->input_name);
  assert(input && "a map's input is a kernel parameter");
  assert(output && "a map kernel fills the tensor passed last");

  bool arithmetic = true;
  auto mapOp = mlir::linalg::MapOp::create(
      builder_, location, /*inputs=*/mlir::ValueRange{input}, /*init=*/output,
      [&](mlir::OpBuilder &b, mlir::Location loc, mlir::ValueRange args) {
        mlir::OpBuilder::InsertionGuard guard(builder_);
        builder_.setInsertionPointToStart(b.getInsertionBlock());
        llvm::StringMap<mlir::Value> bindings;
        bindings[mapExpr->lambda_proto->parameterVectors[0]->name] = args[0];
        auto body =
            mir_.lowerKernelScalar(mapExpr->lambda_body.get(), bindings);
        // The module is dropped then, so the region is left unfinished.
        arithmetic = mlir::succeeded(body);
        if (arithmetic)
          mlir::linalg::YieldOp::create(b, loc, mlir::ValueRange{*body});
      });
  if (!arithmetic) {
    report("A kernel's lambda can only compute on its element with "
           "arithmetic",
           mapExpr->lambda_body->get_location());
    return mlir::failure();
  }
  return mapOp->getResult(0);
}

mlir::FailureOr<mlir::Value>
KernelGen::defineReduce(AST::KernelReduceExprAST *reduceExpr,
                        AST::KernelDefAST *kd, mlir::Location location) {
  auto input = params_.lookup(reduceExpr->input_name);
  auto &params = kd->Prototype->parameterVectors;
  auto param = llvm::find_if(
      params, [&](auto &p) { return p->name == reduceExpr->input_name; });
  assert(input && param != params.end() &&
         "a reduce's input is a kernel parameter");
  auto element =
      std::get<ArrayType>((*param)->get_type().type_data).get_element();

  auto identity = mir_.lowerKernelScalar(reduceExpr->identity.get(), {});
  if (mlir::failed(identity)) {
    report("A reduce's identity can only be arithmetic",
           reduceExpr->identity->get_location());
    return mlir::failure();
  }

  // A 0-d tensor holding the identity, which the reduce folds into.
  auto empty = mlir::tensor::EmptyOp::create(
      builder_, location, llvm::ArrayRef<int64_t>{}, kernelType(element));
  auto init = mlir::linalg::FillOp::create(builder_, location,
                                           mlir::ValueRange{*identity},
                                           mlir::ValueRange{empty.getResult()})
                  .getResultTensors()[0];

  auto reduceOp = mlir::linalg::ReduceOp::create(
      builder_, location, /*inputs=*/mlir::ValueRange{input},
      /*inits=*/mlir::ValueRange{init}, /*dimensions=*/{0},
      [&](mlir::OpBuilder &b, mlir::Location loc, mlir::ValueRange args) {
        auto op = reduceExpr->op_tok->tok_type;
        mlir::Value result =
            isFloat(element) ? emitFloatArithOp(b, loc, args[0], args[1], op)
                             : emitIntArithOp(b, loc, args[0], args[1], op,
                                              isUnsigned(element));
        if (!result)
          internalError("Unsupported reduce operator");
        mlir::linalg::YieldOp::create(b, loc, mlir::ValueRange{result});
      });

  return mlir::tensor::ExtractOp::create(builder_, location,
                                         reduceOp->getResult(0),
                                         mlir::ValueRange{})
      .getResult();
}

// --- The wrapper, between the host and the kernel ---

void KernelGen::defineWrapper(AST::KernelDefAST *kd, mlir::Location location) {
  auto wrapperOp = cpuModule_.lookupSymbol<mlir::func::FuncOp>(publicName(kd));
  assert(wrapperOp && "a kernel's wrapper is declared before it is defined");

  auto &entryBlock = *wrapperOp.addEntryBlock();
  builder_.setInsertionPointToStart(&entryBlock);

  auto result = returnType(kd->Prototype.get());
  bool returnsArray = result.type_kind == TypeKind::Array;

  // An array: on the GPU, a copy on the device; on the CPU, a memref of
  // where it is.
  llvm::SmallVector<mlir::Value> deviceCopies;
  auto passArray = [&](mlir::Value arg, const Type &type) -> mlir::Value {
    if (targetGPU()) {
      auto device =
          smn::ToDeviceOp::create(builder_, location, arg.getType(), arg)
              .getResult();
      deviceCopies.push_back(device);
      return device;
    }
    auto array = std::get<ArrayType>(type.type_data);
    return kernel_abi::memrefOfPointer(builder_, location, arg,
                                       static_cast<int64_t>(array.get_size()),
                                       kernelType(array.get_element()));
  };

  llvm::SmallVector<mlir::Value> kernelArgs;
  for (auto [param, arg] :
       llvm::zip(kd->Prototype->parameterVectors, entryBlock.getArguments())) {
    auto type = param->get_type();
    kernelArgs.push_back(
        type.type_kind == TypeKind::Array ? passArray(arg, type) : arg);
  }
  // The buffer for a returned array, which the kernel fills.
  mlir::Value output;
  if (returnsArray) {
    auto buffer = entryBlock.getArgument(entryBlock.getNumArguments() - 1);
    output = passArray(buffer, result);
    kernelArgs.push_back(output);
    if (targetGPU())
      deviceCopies.pop_back(); // copied back to the host below
  }

  auto memrefType = kernelFunctionType(kd->Prototype.get(), /*asMemref=*/true);
  auto call = mlir::func::CallOp::create(builder_, location, internalName(kd),
                                         memrefType.getResults(), kernelArgs);

  if (targetGPU()) {
    if (returnsArray)
      smn::ToHostOp::create(builder_, location, output.getType(), output);
    for (auto copy : deviceCopies)
      gpuDealloc(copy, location);
  }

  if (returnsArray || result.type_kind == TypeKind::Unit)
    mlir::func::ReturnOp::create(builder_, location);
  else
    mlir::func::ReturnOp::create(builder_, location, call.getResults());
}

void KernelGen::gpuDealloc(mlir::Value deviceMemref, mlir::Location loc) {
  auto tokenType = mlir::gpu::AsyncTokenType::get(builder_.getContext());
  auto wait = mlir::gpu::WaitOp::create(builder_, loc, tokenType,
                                        /*asyncDependencies=*/{});
  auto dealloc = mlir::gpu::DeallocOp::create(
      builder_, loc, tokenType,
      /*asyncDependencies=*/mlir::ValueRange{wait.getAsyncToken()},
      deviceMemref);
  mlir::gpu::WaitOp::create(
      builder_, loc, /*asyncToken=*/mlir::Type{},
      /*asyncDependencies=*/mlir::ValueRange{dealloc.getAsyncToken()});
}

// --- Types ---

mlir::Type KernelGen::kernelType(const Type &type, bool asMemref) {
  switch (type.type_kind) {
  case TypeKind::Array: {
    auto &array = std::get<ArrayType>(type.type_data);
    if (array.get_element().type_kind == TypeKind::Array)
      internalError("Nested arrays not yet supported in kernel context");
    auto element = kernelType(array.get_element());
    auto size = static_cast<int64_t>(array.get_size());
    if (asMemref)
      return mlir::MemRefType::get({size}, element);
    return mlir::RankedTensorType::get({size}, element);
  }
  case TypeKind::I32_t:
  case TypeKind::U32_t:
  case TypeKind::Integer:
    return builder_.getI32Type();
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return builder_.getI64Type();
  case TypeKind::F64_t:
  case TypeKind::Flt:
    return builder_.getF64Type();
  case TypeKind::F32_t:
    return builder_.getF32Type();
  case TypeKind::Bool:
    return builder_.getI1Type();
  case TypeKind::Char:
    return builder_.getI8Type();
  case TypeKind::Unit:
    return mlir::NoneType::get(builder_.getContext());
  case TypeKind::Pointer:
  case TypeKind::Function:
  case TypeKind::String:
  case TypeKind::Struct:
  case TypeKind::Enum:
  case TypeKind::Tuple:
    internalError(fmt::format("Type '{}' is not valid in kernel context",
                              type.to_string()));
  default:
    internalError("Unknown type kind in kernel context");
  }
}

mlir::FunctionType KernelGen::kernelFunctionType(AST::PrototypeAST *proto,
                                                 bool asMemref) {
  llvm::SmallVector<mlir::Type, 4> inputs;
  for (auto &param : proto->parameterVectors)
    inputs.push_back(kernelType(param->get_type(), asMemref));
  llvm::SmallVector<mlir::Type, 1> results;
  auto result = returnType(proto);
  if (result.type_kind != TypeKind::Unit) {
    auto type = kernelType(result, asMemref);
    results.push_back(type);
    if (result.type_kind == TypeKind::Array)
      inputs.push_back(type);
  }
  return builder_.getFunctionType(inputs, results);
}

mlir::FunctionType KernelGen::wrapperFunctionType(AST::PrototypeAST *proto) {
  auto array = [&](const Type &type) -> mlir::Type {
    if (targetGPU())
      return kernelType(type, /*asMemref=*/true);
    return mlir::LLVM::LLVMPointerType::get(builder_.getContext());
  };
  llvm::SmallVector<mlir::Type, 4> inputs;
  for (auto &param : proto->parameterVectors) {
    auto type = param->get_type();
    inputs.push_back(type.type_kind == TypeKind::Array ? array(type)
                                                       : kernelType(type));
  }
  llvm::SmallVector<mlir::Type, 1> results;
  auto result = returnType(proto);
  if (result.type_kind == TypeKind::Array)
    inputs.push_back(array(result));
  else if (result.type_kind != TypeKind::Unit)
    results.push_back(kernelType(result));
  return builder_.getFunctionType(inputs, results);
}

} // namespace sammine_lang
