#include "dialect/MIRToLLVM.h"
#include "cabi/CABI.h"
#include "cabi/KernelABI.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Transforms/FuncConversions.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Transforms/DialectConversion.h"

namespace sammine_lang {

namespace {

/// LLVM's opaque pointer, what each of MIR's pointers and places becomes.
mlir::LLVM::LLVMPointerType llvmPtr(mlir::MLIRContext *ctx) {
  return mlir::LLVM::LLVMPointerType::get(ctx);
}

/// A function value: the code to call, which takes
/// the environment first, and the environment.
mlir::LLVM::LLVMStructType closureType(mlir::MLIRContext *ctx) {
  auto type = mlir::LLVM::LLVMStructType::getIdentified(ctx, "sammine.closure");
  if (!type.isInitialized()) {
    auto ptr = llvmPtr(ctx);
    (void)type.setBody({ptr, ptr}, /*isPacked=*/false);
  }
  return type;
}

/// MIR's types as LLVM's: a pointer, owned or a view, and a place are each an
/// !llvm.ptr; a struct is the identified LLVM struct `sammine.struct.<name>`,
/// which declarations of functions taking or giving one use too, and a
/// tuple a literal one. Any other type is
/// left as it is.
class MIRTypeConverter : public mlir::TypeConverter {
public:
  MIRTypeConverter() {
    addConversion([](mlir::Type type) { return type; });
    addConversion([](smn::PtrType type) -> mlir::Type {
      return llvmPtr(type.getContext());
    });
    addConversion([](smn::PlaceType type) -> mlir::Type {
      return llvmPtr(type.getContext());
    });
    addConversion([this](smn::StructType type) -> std::optional<mlir::Type> {
      // Identified by name, so the same type as declarations use, whose
      // fields may be set already.
      auto llvmType = mlir::LLVM::LLVMStructType::getIdentified(
          type.getContext(), ("sammine.struct." + type.getName()).str());
      if (llvmType.isInitialized())
        return llvmType;
      llvm::SmallVector<mlir::Type> fields;
      if (mlir::failed(convertTypes(type.getFields(), fields)) ||
          mlir::failed(llvmType.setBody(fields, /*isPacked=*/false)))
        return std::nullopt;
      return llvmType;
    });
    addConversion([](smn::EnumType type) -> mlir::Type {
      // { i32 tag, [N x i8] payload }, or just the tag with no payload.
      auto *ctx = type.getContext();
      auto llvmType = mlir::LLVM::LLVMStructType::getIdentified(
          ctx, ("sammine.enum." + type.getName()).str());
      if (llvmType.isInitialized())
        return llvmType;
      llvm::SmallVector<mlir::Type, 2> fields{mlir::IntegerType::get(ctx, 32)};
      if (type.getPayloadSize() > 0)
        fields.push_back(mlir::LLVM::LLVMArrayType::get(
            mlir::IntegerType::get(ctx, 8),
            static_cast<uint64_t>(type.getPayloadSize())));
      (void)llvmType.setBody(fields, /*isPacked=*/false);
      return llvmType;
    });
    addConversion([this](smn::ArrayType type) -> std::optional<mlir::Type> {
      auto element = convertType(type.getElement());
      if (!element)
        return std::nullopt;
      return mlir::LLVM::LLVMArrayType::get(
          element, static_cast<unsigned>(type.getSize()));
    });
    addConversion([](smn::FnType type) -> mlir::Type {
      return closureType(type.getContext());
    });
    addConversion([this](smn::TupleType type) -> std::optional<mlir::Type> {
      llvm::SmallVector<mlir::Type> elements;
      if (mlir::failed(convertTypes(type.getElements(), elements)))
        return std::nullopt;
      return mlir::LLVM::LLVMStructType::getLiteral(type.getContext(),
                                                    elements);
    });
    // Where a converted value meets one that isn't converted yet.
    auto cast = [](mlir::OpBuilder &builder, mlir::Type type,
                   mlir::ValueRange inputs, mlir::Location loc) -> mlir::Value {
      return mlir::UnrealizedConversionCastOp::create(builder, loc, type,
                                                      inputs)
          .getResult(0);
    };
    addSourceMaterialization(cast);
    addTargetMaterialization(cast);
  }
};

/// The C function `name` of `type`, declared at the top of the module if it
/// isn't yet. The compiler declares malloc and free itself.
void declareCFunction(mlir::ModuleOp module, mlir::OpBuilder &builder,
                      llvm::StringRef name, mlir::LLVM::LLVMFunctionType type) {
  if (module.lookupSymbol(name))
    return;
  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(module.getBody());
  mlir::LLVM::LLVMFuncOp::create(builder, module.getLoc(), name, type);
}

/// A stack slot for one `type`, at the top of the function `op` is in, where
/// LLVM's mem2reg can remove it.
mlir::Value entrySlot(mlir::ConversionPatternRewriter &rewriter,
                      mlir::Operation *op, mlir::Type type) {
  mlir::OpBuilder::InsertionGuard guard(rewriter);
  auto &entry = op->getParentOfType<mlir::FunctionOpInterface>()
                    .getFunctionBody()
                    .front();
  rewriter.setInsertionPointToStart(&entry);
  auto one = mlir::arith::ConstantIntOp::create(rewriter, op->getLoc(),
                                                rewriter.getI64Type(), 1);
  return mlir::LLVM::AllocaOp::create(rewriter, op->getLoc(),
                                      llvmPtr(op->getContext()), type, one);
}

// alloc<T>(n): malloc(sizeof(T) * n), n widened to i64.
struct AllocLowering : public mlir::OpConversionPattern<smn::AllocOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::AllocOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto ptrTy = llvmPtr(op.getContext());
    auto i64Ty = rewriter.getI64Type();
    auto pointee = getTypeConverter()->convertType(
        mlir::cast<smn::PtrType>(op.getType()).getPointee());
    if (!pointee)
      return mlir::failure();
    auto size = static_cast<int64_t>(
        mlir::DataLayout::closest(op).getTypeSize(pointee).getFixedValue());

    mlir::Value count = adaptor.getCount();
    if (count.getType() != i64Ty)
      count = mlir::arith::ExtSIOp::create(rewriter, loc, i64Ty, count);
    auto sizeVal =
        mlir::arith::ConstantIntOp::create(rewriter, loc, i64Ty, size);
    auto bytes = mlir::arith::MulIOp::create(rewriter, loc, sizeVal, count);

    declareCFunction(op->getParentOfType<mlir::ModuleOp>(), rewriter, "malloc",
                     mlir::LLVM::LLVMFunctionType::get(ptrTy, {i64Ty}));
    auto call =
        mlir::LLVM::CallOp::create(rewriter, loc, mlir::TypeRange{ptrTy},
                                   "malloc", mlir::ValueRange{bytes});
    rewriter.replaceOp(op, call.getResult());
    return mlir::success();
  }
};

struct FreeLowering : public mlir::OpConversionPattern<smn::FreeOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::FreeOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto ptrTy = llvmPtr(op.getContext());
    declareCFunction(
        op->getParentOfType<mlir::ModuleOp>(), rewriter, "free",
        mlir::LLVM::LLVMFunctionType::get(
            mlir::LLVM::LLVMVoidType::get(op.getContext()), {ptrTy}));
    mlir::LLVM::CallOp::create(rewriter, op.getLoc(), mlir::TypeRange{}, "free",
                               mlir::ValueRange{adaptor.getPtr()});
    rewriter.eraseOp(op);
    return mlir::success();
  }
};

// A place is where its pointer points, so *p is p itself, and a view lent of
// a place is its address, the same pointer again.
template <typename AddressOp>
struct SameAddressLowering : public mlir::OpConversionPattern<AddressOp> {
  using mlir::OpConversionPattern<AddressOp>::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(AddressOp op, typename AddressOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOp(op, adaptor.getOperands().front());
    return mlir::success();
  }
};

// An element past a pointer: a GEP over its pointee's type.
struct PtrElemLowering : public mlir::OpConversionPattern<smn::PtrElemOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::PtrElemOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto element = getTypeConverter()->convertType(
        mlir::cast<smn::PtrType>(op.getPtr().getType()).getPointee());
    if (!element)
      return mlir::failure();
    rewriter.replaceOpWithNewOp<mlir::LLVM::GEPOp>(
        op, llvmPtr(op.getContext()), element, adaptor.getPtr(),
        llvm::ArrayRef<mlir::LLVM::GEPArg>{adaptor.getIndex()},
        mlir::LLVM::GEPNoWrapFlags::inbounds);
    return mlir::success();
  }
};

// A field's or an element's place: a GEP into what the base place holds.
struct FieldLowering : public mlir::OpConversionPattern<smn::FieldOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::FieldOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto base =
        getTypeConverter()->convertType(op.getBase().getType().getValueType());
    if (!base)
      return mlir::failure();
    rewriter.replaceOpWithNewOp<mlir::LLVM::GEPOp>(
        op, llvmPtr(op.getContext()), base, adaptor.getBase(),
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0,
                                           static_cast<int32_t>(op.getIndex())},
        mlir::LLVM::GEPNoWrapFlags::inbounds);
    return mlir::success();
  }
};

struct ElemLowering : public mlir::OpConversionPattern<smn::ElemOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::ElemOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto base =
        getTypeConverter()->convertType(op.getBase().getType().getValueType());
    if (!base)
      return mlir::failure();
    rewriter.replaceOpWithNewOp<mlir::LLVM::GEPOp>(
        op, llvmPtr(op.getContext()), base, adaptor.getBase(),
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, adaptor.getIndex()},
        mlir::LLVM::GEPNoWrapFlags::inbounds);
    return mlir::success();
  }
};

// Copying and moving both read the place; what differs is only what the
// ownership check makes of them.
template <typename ReadOp>
struct ReadLowering : public mlir::OpConversionPattern<ReadOp> {
  using mlir::OpConversionPattern<ReadOp>::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(ReadOp op, typename ReadOp::Adaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto type = this->getTypeConverter()->convertType(op.getType());
    if (!type)
      return mlir::failure();
    rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(op, type,
                                                    adaptor.getPlace());
    return mlir::success();
  }
};

struct StoreLowering : public mlir::OpConversionPattern<smn::StoreOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::StoreOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<mlir::LLVM::StoreOp>(op, adaptor.getValue(),
                                                     adaptor.getPlace());
    return mlir::success();
  }
};

// A function's signature and the arguments of every block in it: an if's
// merge block takes MIR types too. MLIR's own pattern for function types
// (populateFunctionOpInterfaceTypeConversionPattern) converts only the entry
// block.
struct FuncLowering : public mlir::OpConversionPattern<mlir::func::FuncOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(mlir::func::FuncOp func, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto type = func.getFunctionType();
    auto &converter = *getTypeConverter();
    mlir::TypeConverter::SignatureConversion inputs(type.getNumInputs());
    llvm::SmallVector<mlir::Type, 1> results;
    if (mlir::failed(
            converter.convertSignatureArgs(type.getInputs(), inputs)) ||
        mlir::failed(converter.convertTypes(type.getResults(), results)))
      return mlir::failure();
    if (!func.isExternal() && mlir::failed(rewriter.convertRegionTypes(
                                  &func.getBody(), converter, &inputs)))
      return mlir::failure();
    rewriter.modifyOpInPlace(func, [&] {
      func.setType(
          rewriter.getFunctionType(inputs.getConvertedTypes(), results));
    });
    return mlir::success();
  }
};

// By now the callee has its final signature in LLVM types, whichever way it
// was emitted, and MIR's call takes and gives the same ones once converted. A
// C function declared as an llvm.func is called as C calls it, a kernel's
// wrapper as the kernel codegen made it.
struct CallLowering : public mlir::OpConversionPattern<smn::CallOp> {
  using OpConversionPattern::OpConversionPattern;

  // A kernel's wrapper takes an array by pointer on the CPU and as a memref
  // on the GPU, as its parameter's type says: the array goes through a stack
  // slot. An array it returns, it fills in a buffer passed last.
  static mlir::LogicalResult
  callKernel(smn::CallOp op, mlir::FunctionOpInterface callee,
             mlir::ValueRange args, llvm::ArrayRef<mlir::Type> results,
             mlir::ConversionPatternRewriter &rewriter) {
    auto loc = op.getLoc();
    auto params = callee.getArgumentTypes();
    // A slot holding an array, as `param`: its address, or a memref of it.
    auto passArray = [&](mlir::Value slot, mlir::Type param) -> mlir::Value {
      if (auto memref = mlir::dyn_cast<mlir::MemRefType>(param))
        return kernel_abi::memrefOfPointer(
            rewriter, loc, slot, memref.getDimSize(0), memref.getElementType());
      return slot;
    };
    llvm::SmallVector<mlir::Value, 4> operands;
    for (auto [arg, param] : llvm::zip(args, params)) {
      auto array = mlir::dyn_cast<mlir::LLVM::LLVMArrayType>(arg.getType());
      if (!array || arg.getType() == param) {
        operands.push_back(arg);
        continue;
      }
      auto slot = entrySlot(rewriter, op, array);
      mlir::LLVM::StoreOp::create(rewriter, loc, arg, slot);
      operands.push_back(passArray(slot, param));
    }
    mlir::Value buffer;
    bool returnsInBuffer =
        !params.empty() &&
        callee.getArgAttr(static_cast<unsigned>(params.size() - 1),
                          smn::kResultBufferAttr);
    if (returnsInBuffer) {
      auto array = mlir::dyn_cast<mlir::LLVM::LLVMArrayType>(results.front());
      if (!array)
        return mlir::failure();
      buffer = entrySlot(rewriter, op, array);
      operands.push_back(passArray(buffer, params.back()));
    }
    auto call = mlir::func::CallOp::create(rewriter, loc, op.getCalleeAttr(),
                                           callee.getResultTypes(), operands);
    if (returnsInBuffer)
      rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(op, results.front(),
                                                      buffer);
    else
      rewriter.replaceOp(op, call.getResults());
    return mlir::success();
  }
  mlir::LogicalResult
  matchAndRewrite(smn::CallOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    llvm::SmallVector<mlir::Type, 1> results;
    if (mlir::failed(
            getTypeConverter()->convertTypes(op.getResultTypes(), results)))
      return mlir::failure();
    auto callee =
        mlir::SymbolTable::lookupNearestSymbolFrom<mlir::FunctionOpInterface>(
            op, op.getCalleeAttr());
    if (callee && callee->hasAttr("sammine.kernel_wrapper"))
      return callKernel(op, callee, adaptor.getArgs(), results, rewriter);
    if (auto cFunc =
            mlir::SymbolTable::lookupNearestSymbolFrom<mlir::LLVM::LLVMFuncOp>(
                op, op.getCalleeAttr())) {
      auto value = cabi::callC(
          rewriter, op.getLoc(), cFunc, llvm::to_vector<4>(adaptor.getArgs()),
          results.empty() ? mlir::Type() : results.front(),
          [&](mlir::Type type) { return entrySlot(rewriter, op, type); });
      if (value)
        rewriter.replaceOp(op, value);
      else
        rewriter.eraseOp(op);
      return mlir::success();
    }
    rewriter.replaceOpWithNewOp<mlir::func::CallOp>(op, op.getCalleeAttr(),
                                                    results, adaptor.getArgs());
    return mlir::success();
  }
};

/// The constant array `op` builds, if every element is a number constant.
std::optional<mlir::DenseElementsAttr> constantArray(smn::AggregateOp op,
                                                     mlir::Type type) {
  auto array = mlir::dyn_cast<mlir::LLVM::LLVMArrayType>(type);
  if (!array ||
      !mlir::isa<mlir::IntegerType, mlir::FloatType>(array.getElementType()))
    return std::nullopt;
  llvm::SmallVector<mlir::Attribute> elements;
  for (auto field : op.getFields()) {
    auto constant = field.getDefiningOp<mlir::arith::ConstantOp>();
    if (!constant)
      return std::nullopt;
    elements.push_back(constant.getValue());
  }
  return mlir::DenseElementsAttr::get(
      mlir::RankedTensorType::get({static_cast<int64_t>(elements.size())},
                                  array.getElementType()),
      elements);
}

// A struct, tuple or array is built field by field into an LLVM aggregate.
// An array of number constants is a constant global instead, loaded whole.
struct AggregateLowering : public mlir::OpConversionPattern<smn::AggregateOp> {
  AggregateLowering(const mlir::TypeConverter &converter,
                    mlir::MLIRContext *context, unsigned &constArrays)
      : OpConversionPattern(converter, context), constArrays(constArrays) {}

  mlir::LogicalResult
  matchAndRewrite(smn::AggregateOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto type = getTypeConverter()->convertType(op.getType());
    if (!type)
      return mlir::failure();
    if (auto elements = constantArray(op, type)) {
      mlir::LLVM::GlobalOp global;
      {
        mlir::OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToStart(
            op->getParentOfType<mlir::ModuleOp>().getBody());
        global = mlir::LLVM::GlobalOp::create(
            rewriter, op.getLoc(), type, /*isConstant=*/true,
            mlir::LLVM::Linkage::Internal,
            (".mir.const_arr." + llvm::Twine(constArrays++)).str(), *elements);
      }
      auto address =
          mlir::LLVM::AddressOfOp::create(rewriter, op.getLoc(), global);
      rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(op, type, address);
      return mlir::success();
    }
    mlir::Value result =
        mlir::LLVM::PoisonOp::create(rewriter, op.getLoc(), type);
    for (auto [i, field] : llvm::enumerate(adaptor.getFields()))
      result = mlir::LLVM::InsertValueOp::create(
          rewriter, op.getLoc(), result, field,
          llvm::ArrayRef<int64_t>{static_cast<int64_t>(i)});
    rewriter.replaceOp(op, result);
    return mlir::success();
  }

private:
  unsigned &constArrays;
};

struct ExtractLowering : public mlir::OpConversionPattern<smn::ExtractOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::ExtractOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<mlir::LLVM::ExtractValueOp>(
        op, adaptor.getAggregate(),
        llvm::ArrayRef<int64_t>{static_cast<int64_t>(op.getIndex())});
    return mlir::success();
  }
};

// A slot at the top of the function.
struct LocalLowering : public mlir::OpConversionPattern<smn::LocalOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::LocalOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto type =
        getTypeConverter()->convertType(op.getPlace().getType().getValueType());
    if (!type)
      return mlir::failure();
    rewriter.replaceOp(op, entrySlot(rewriter, op, type));
    return mlir::success();
  }
};

/// Where the payload field at byte `offset` of the enum in `slot` is.
mlir::Value payloadField(mlir::ConversionPatternRewriter &rewriter,
                         mlir::Location loc, mlir::Type enumType,
                         mlir::Value slot, int64_t offset) {
  auto ptrTy = llvmPtr(rewriter.getContext());
  mlir::Value payload =
      mlir::LLVM::GEPOp::create(rewriter, loc, ptrTy, enumType, slot,
                                llvm::ArrayRef<mlir::LLVM::GEPArg>{0, 1},
                                mlir::LLVM::GEPNoWrapFlags::inbounds);
  if (offset == 0)
    return payload;
  return mlir::LLVM::GEPOp::create(
      rewriter, loc, ptrTy, rewriter.getI8Type(), payload,
      llvm::ArrayRef<mlir::LLVM::GEPArg>{static_cast<int32_t>(offset)},
      mlir::LLVM::GEPNoWrapFlags::inbounds);
}

// An enum value is built in memory: its tag, then each payload field at its
// offset, then loaded whole.
struct VariantLowering : public mlir::OpConversionPattern<smn::VariantOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::VariantOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto enumType = getTypeConverter()->convertType(op.getType());
    if (!enumType)
      return mlir::failure();
    auto slot = entrySlot(rewriter, op, enumType);
    auto tag =
        mlir::arith::ConstantIntOp::create(rewriter, loc, rewriter.getI32Type(),
                                           static_cast<int64_t>(op.getIndex()));
    auto tagPtr = mlir::LLVM::GEPOp::create(
        rewriter, loc, llvmPtr(op.getContext()), enumType, slot,
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, 0},
        mlir::LLVM::GEPNoWrapFlags::inbounds);
    mlir::LLVM::StoreOp::create(rewriter, loc, tag, tagPtr);
    for (auto [field, offset] :
         llvm::zip(adaptor.getPayload(), op.getOffsets()))
      mlir::LLVM::StoreOp::create(
          rewriter, loc, field,
          payloadField(rewriter, loc, enumType, slot, offset));
    rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(op, enumType, slot);
    return mlir::success();
  }
};

struct TagLowering : public mlir::OpConversionPattern<smn::TagOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::TagOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<mlir::LLVM::ExtractValueOp>(
        op, adaptor.getValue(), llvm::ArrayRef<int64_t>{0});
    return mlir::success();
  }
};

// A payload field is read through memory, from its offset.
struct PayloadLowering : public mlir::OpConversionPattern<smn::PayloadOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::PayloadOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto enumType = adaptor.getValue().getType();
    auto fieldType = getTypeConverter()->convertType(op.getType());
    if (!fieldType)
      return mlir::failure();
    auto slot = entrySlot(rewriter, op, enumType);
    mlir::LLVM::StoreOp::create(rewriter, loc, adaptor.getValue(), slot);
    rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(
        op, fieldType,
        payloadField(rewriter, loc, enumType, slot,
                     static_cast<int64_t>(op.getOffset())));
    return mlir::success();
  }
};

// An element of an array value at an index only known when it runs: through
// a stack slot.
struct ElementLowering : public mlir::OpConversionPattern<smn::ElementOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::ElementOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto arrayType = adaptor.getArray().getType();
    auto element = getTypeConverter()->convertType(op.getType());
    if (!element)
      return mlir::failure();
    auto slot = entrySlot(rewriter, op, arrayType);
    mlir::LLVM::StoreOp::create(rewriter, op.getLoc(), adaptor.getArray(),
                                slot);
    auto at = mlir::LLVM::GEPOp::create(
        rewriter, op.getLoc(), llvmPtr(op.getContext()), arrayType, slot,
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, adaptor.getIndex()},
        mlir::LLVM::GEPNoWrapFlags::inbounds);
    rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(op, element, at);
    return mlir::success();
  }
};

struct AddressLowering : public mlir::OpConversionPattern<smn::AddressOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::AddressOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<mlir::LLVM::PtrToIntOp>(
        op, rewriter.getI64Type(), adaptor.getPtr());
    return mlir::success();
  }
};

struct UnreachableLowering
    : public mlir::OpConversionPattern<smn::UnreachableOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::UnreachableOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<mlir::LLVM::UnreachableOp>(op);
    return mlir::success();
  }
};

// A function value: the code is a wrapper taking the environment first,
// then the arguments not bound, which calls the callee with the bound ones
// before them. A function's own value binds nothing and shares one wrapper,
// __wrap_<f>, with a null environment; a
// partial application's environment holds what it binds, in a stack slot
// of the function that makes it, read by a wrapper of its own.
struct ClosureLowering : public mlir::OpConversionPattern<smn::ClosureOp> {
  ClosureLowering(const mlir::TypeConverter &converter,
                  mlir::MLIRContext *context, unsigned &partials)
      : OpConversionPattern(converter, context), partials(partials) {}

  mlir::LogicalResult
  matchAndRewrite(smn::ClosureOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto *ctx = op.getContext();
    auto module = op->getParentOfType<mlir::ModuleOp>();
    auto ptrTy = llvmPtr(ctx);
    auto signature = op.getType().getSignature();
    llvm::SmallVector<mlir::Type> rest, results;
    if (mlir::failed(
            getTypeConverter()->convertTypes(signature.getInputs(), rest)) ||
        mlir::failed(
            getTypeConverter()->convertTypes(signature.getResults(), results)))
      return mlir::failure();
    llvm::SmallVector<mlir::Type> wrapperParams{ptrTy};
    wrapperParams.append(rest);
    auto wrapperType = mlir::LLVM::LLVMFunctionType::get(
        results.empty() ? mlir::LLVM::LLVMVoidType::get(ctx) : results.front(),
        wrapperParams);

    auto bound = adaptor.getBound();
    mlir::Value env;
    mlir::LLVM::LLVMStructType envType;
    std::string name;
    if (bound.empty()) {
      env = mlir::LLVM::ZeroOp::create(rewriter, loc, ptrTy);
      name = ("__wrap_" + op.getCallee()).str();
    } else {
      envType = mlir::LLVM::LLVMStructType::getLiteral(
          ctx, llvm::to_vector(bound.getTypes()));
      env = entrySlot(rewriter, op, envType);
      for (auto [i, value] : llvm::enumerate(bound))
        mlir::LLVM::StoreOp::create(
            rewriter, loc, value,
            mlir::LLVM::GEPOp::create(
                rewriter, loc, ptrTy, envType, env,
                llvm::ArrayRef<mlir::LLVM::GEPArg>{0, static_cast<int32_t>(i)},
                mlir::LLVM::GEPNoWrapFlags::inbounds));
      do
        name = "__partial_" + std::to_string(partials++);
      while (module.lookupSymbol(name));
    }

    if (!module.lookupSymbol(name)) {
      mlir::OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToEnd(module.getBody());
      auto wrapper = mlir::LLVM::LLVMFuncOp::create(
          rewriter, loc, name, wrapperType, mlir::LLVM::Linkage::Internal);
      auto *entry = wrapper.addEntryBlock(rewriter);
      rewriter.setInsertionPointToStart(entry);
      llvm::SmallVector<mlir::Value, 4> args;
      for (auto [i, value] : llvm::enumerate(bound))
        args.push_back(mlir::LLVM::LoadOp::create(
            rewriter, loc, value.getType(),
            mlir::LLVM::GEPOp::create(
                rewriter, loc, ptrTy, envType, entry->getArgument(0),
                llvm::ArrayRef<mlir::LLVM::GEPArg>{0, static_cast<int32_t>(i)},
                mlir::LLVM::GEPNoWrapFlags::inbounds)));
      for (auto arg : entry->getArguments().drop_front())
        args.push_back(arg);
      if (auto cFunc =
              module.lookupSymbol<mlir::LLVM::LLVMFuncOp>(op.getCallee())) {
        // A slot for C's struct passing, at the top of the wrapper.
        auto slot = [&](mlir::Type type) -> mlir::Value {
          mlir::OpBuilder::InsertionGuard top(rewriter);
          rewriter.setInsertionPointToStart(entry);
          auto one = mlir::arith::ConstantIntOp::create(
              rewriter, loc, rewriter.getI64Type(), 1);
          return mlir::LLVM::AllocaOp::create(rewriter, loc, ptrTy, type, one);
        };
        auto value =
            cabi::callC(rewriter, loc, cFunc, args,
                        results.empty() ? mlir::Type() : results.front(), slot);
        mlir::LLVM::ReturnOp::create(rewriter, loc,
                                     value ? mlir::ValueRange{value}
                                           : mlir::ValueRange{});
      } else {
        auto call = mlir::func::CallOp::create(
            rewriter, loc, op.getCalleeAttr(), results, args);
        mlir::LLVM::ReturnOp::create(rewriter, loc, call.getResults());
      }
    }

    auto closure = closureType(ctx);
    mlir::Value value = mlir::LLVM::PoisonOp::create(rewriter, loc, closure);
    auto code = mlir::LLVM::AddressOfOp::create(rewriter, loc, ptrTy, name);
    value = mlir::LLVM::InsertValueOp::create(rewriter, loc, value, code,
                                              llvm::ArrayRef<int64_t>{0});
    value = mlir::LLVM::InsertValueOp::create(rewriter, loc, value, env,
                                              llvm::ArrayRef<int64_t>{1});
    rewriter.replaceOp(op, value);
    return mlir::success();
  }

private:
  unsigned &partials;
};

// A call through a function value: its code, passed its environment first.
struct CallIndirectLowering
    : public mlir::OpConversionPattern<smn::CallIndirectOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::CallIndirectOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto *ctx = op.getContext();
    auto ptrTy = llvmPtr(ctx);
    llvm::SmallVector<mlir::Type> results;
    if (mlir::failed(
            getTypeConverter()->convertTypes(op.getResultTypes(), results)))
      return mlir::failure();
    auto code = mlir::LLVM::ExtractValueOp::create(
        rewriter, loc, ptrTy, adaptor.getCallee(), llvm::ArrayRef<int64_t>{0});
    auto env = mlir::LLVM::ExtractValueOp::create(
        rewriter, loc, ptrTy, adaptor.getCallee(), llvm::ArrayRef<int64_t>{1});
    llvm::SmallVector<mlir::Type> params{ptrTy};
    llvm::SmallVector<mlir::Value> operands{code, env};
    for (auto arg : adaptor.getArgs()) {
      params.push_back(arg.getType());
      operands.push_back(arg);
    }
    auto type = mlir::LLVM::LLVMFunctionType::get(
        results.empty() ? mlir::LLVM::LLVMVoidType::get(ctx) : results.front(),
        params);
    auto call = mlir::LLVM::CallOp::create(rewriter, loc, type, operands);
    rewriter.replaceOp(op, call.getResults());
    return mlir::success();
  }
};

// A string literal is the address of a constant global holding it, one per
// distinct string in the module.
struct StringLowering : public mlir::OpConversionPattern<smn::StringOp> {
  StringLowering(const mlir::TypeConverter &converter,
                 mlir::MLIRContext *context,
                 llvm::StringMap<mlir::LLVM::GlobalOp> &globals)
      : OpConversionPattern(converter, context), globals(globals) {}

  mlir::LogicalResult
  matchAndRewrite(smn::StringOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto &global = globals[op.getValue()];
    if (!global)
      global = cabi::createString(
          rewriter, op->getParentOfType<mlir::ModuleOp>(), op.getLoc(),
          (".mir.str." + llvm::Twine(globals.size() - 1)).str(), op.getValue());
    rewriter.replaceOpWithNewOp<mlir::LLVM::AddressOfOp>(op, global);
    return mlir::success();
  }

private:
  llvm::StringMap<mlir::LLVM::GlobalOp> &globals;
};

struct MIRToLLVMPass
    : public mlir::PassWrapper<MIRToLLVMPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(MIRToLLVMPass)

  llvm::StringRef getArgument() const override { return "sammine-mir-to-llvm"; }
  llvm::StringRef getDescription() const override {
    return "Convert MIR's types and ops to the LLVM dialect";
  }
  void getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<mlir::LLVM::LLVMDialect, mlir::arith::ArithDialect>();
  }

  void runOnOperation() override {
    auto &context = getContext();
    MIRTypeConverter converter;

    mlir::RewritePatternSet patterns(&context);
    mlir::populateCallOpTypeConversionPattern(patterns, converter);
    mlir::populateReturnOpTypeConversionPattern(patterns, converter);
    mlir::populateBranchOpInterfaceTypeConversionPattern(patterns, converter);
    patterns.add<
        FuncLowering, AllocLowering, FreeLowering,
        SameAddressLowering<smn::DerefOp>, SameAddressLowering<smn::LendOp>,
        SameAddressLowering<smn::AddrOp>, PtrElemLowering, FieldLowering,
        ElemLowering, ReadLowering<smn::CopyOp>, ReadLowering<smn::MoveOp>,
        StoreLowering, CallLowering, ExtractLowering, VariantLowering,
        TagLowering, PayloadLowering, ElementLowering, UnreachableLowering,
        LocalLowering, CallIndirectLowering, AddressLowering>(converter,
                                                              &context);
    unsigned partials = 0;
    patterns.add<ClosureLowering>(converter, &context, partials);
    llvm::StringMap<mlir::LLVM::GlobalOp> strings;
    patterns.add<StringLowering>(converter, &context, strings);
    unsigned constArrays = 0;
    patterns.add<AggregateLowering>(converter, &context, constArrays);

    mlir::ConversionTarget target(context);
    target.addDynamicallyLegalOp<mlir::func::FuncOp>(
        [&](mlir::func::FuncOp func) {
          return converter.isSignatureLegal(func.getFunctionType()) &&
                 converter.isLegal(&func.getBody());
        });
    target.addLegalOp<mlir::UnrealizedConversionCastOp>();
    // Every MIR op is lowered here. to_device and to_host aren't MIR: the
    // kernel wrappers' ops, which lower-sammine-ops lowers before this pass.
    target.addIllegalDialect<smn::SammineDialect>();
    target.addLegalOp<smn::ToDeviceOp, smn::ToHostOp>();
    // Any other op only needs converting if it takes or gives a MIR type:
    // most of a module, e.g. llvm.unreachable, has nothing to do with MIR.
    target.markUnknownOpDynamicallyLegal(
        [&](mlir::Operation *op) { return converter.isLegal(op); });

    if (mlir::failed(mlir::applyPartialConversion(getOperation(), target,
                                                  std::move(patterns))))
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createMIRToLLVMPass() {
  return std::make_unique<MIRToLLVMPass>();
}

void registerMIRToLLVMPass() { mlir::PassRegistration<MIRToLLVMPass>(); }

} // namespace sammine_lang
