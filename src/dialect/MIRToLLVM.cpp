#include "dialect/MIRToLLVM.h"
#include "cabi/CABI.h"
#include "cabi/KernelABI.h"
#include "dialect/ElaborateDrops.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/Transforms/StructuralTypeConversions.h"
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

/// The words a value of `fn` carries what it binds in, if it has any.
mlir::Type wordsType(smn::FnType fn) {
  return mlir::LLVM::LLVMArrayType::get(
      mlir::IntegerType::get(fn.getContext(), 64),
      static_cast<unsigned>(fn.getWords()));
}

/// A function value: the code to call, then the words of what it binds,
/// which the code is passed a pointer to first. With no words, the code.
mlir::LLVM::LLVMStructType closureType(smn::FnType fn) {
  auto *ctx = fn.getContext();
  llvm::SmallVector<mlir::Type, 2> fields{llvmPtr(ctx)};
  if (fn.getWords() > 0)
    fields.push_back(wordsType(fn));
  return mlir::LLVM::LLVMStructType::getLiteral(ctx, fields);
}

/// How the code of a value of `fn` takes its words, first: by value, so
/// a call through a closure leaves no address of the caller's stack to
/// the code (LLVM wouldn't make a tail call of the caller's own then), or a
/// null pointer when it has none.
mlir::Type wordsParam(smn::FnType fn) {
  return fn.getWords() > 0 ? wordsType(fn)
                           : mlir::Type(llvmPtr(fn.getContext()));
}

/// Where field `index` of the aggregate of `type` at `base` is.
mlir::Value fieldPtr(mlir::OpBuilder &builder, mlir::Location loc,
                     mlir::Type type, mlir::Value base,
                     mlir::LLVM::GEPArg index) {
  return mlir::LLVM::GEPOp::create(builder, loc, llvmPtr(builder.getContext()),
                                   type, base,
                                   llvm::ArrayRef<mlir::LLVM::GEPArg>{0, index},
                                   mlir::LLVM::GEPNoWrapFlags::inbounds);
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
    addConversion(
        [](smn::FnType type) -> mlir::Type { return closureType(type); });
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

/// A stack slot for one `type`, at the top of `entry`, where LLVM's mem2reg
/// can remove it.
mlir::Value entrySlot(mlir::OpBuilder &builder, mlir::Block &entry,
                      mlir::Location loc, mlir::Type type) {
  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(&entry);
  auto one =
      mlir::arith::ConstantIntOp::create(builder, loc, builder.getI64Type(), 1);
  return mlir::LLVM::AllocaOp::create(builder, loc,
                                      llvmPtr(builder.getContext()), type, one);
}

/// A stack slot for one `type`, at the top of the function `op` is in.
mlir::Value entrySlot(mlir::OpBuilder &builder, mlir::Operation *op,
                      mlir::Type type) {
  return entrySlot(builder,
                   op->getParentOfType<mlir::FunctionOpInterface>()
                       .getFunctionBody()
                       .front(),
                   op->getLoc(), type);
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

// Dropping an owned pointer frees its memory.
struct DropLowering : public mlir::OpConversionPattern<smn::DropOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::DropOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto ptrTy = llvmPtr(op.getContext());
    declareCFunction(
        op->getParentOfType<mlir::ModuleOp>(), rewriter, "free",
        mlir::LLVM::LLVMFunctionType::get(
            mlir::LLVM::LLVMVoidType::get(op.getContext()), {ptrTy}));
    mlir::LLVM::CallOp::create(rewriter, op.getLoc(), mlir::TypeRange{}, "free",
                               mlir::ValueRange{adaptor.getValue()});
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

// The place a pointer held in a place points at: the pointer, loaded.
struct ThroughLowering : public mlir::OpConversionPattern<smn::ThroughOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::ThroughOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(
        op, llvmPtr(op.getContext()), adaptor.getPlace());
    return mlir::success();
  }
};

// A binding is the value it names.
struct LetLowering : public mlir::OpConversionPattern<smn::LetOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::LetOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOp(op, adaptor.getValue());
    return mlir::success();
  }
};

// An element past a pointer: a GEP over its pointee's type.
struct PtrElemLowering : public mlir::OpConversionPattern<smn::PtrElemOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::PtrElemOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto element =
        getTypeConverter()->convertType(op.getBase().getType().getValueType());
    if (!element)
      return mlir::failure();
    rewriter.replaceOpWithNewOp<mlir::LLVM::GEPOp>(
        op, llvmPtr(op.getContext()), element, adaptor.getBase(),
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

/// The function `name` of `type`, defined at the end of `module` unless it
/// is there already: it calls `callee` with what `leading` gives, then its
/// own arguments from `firstOwn` on, and returns what `callee` returns
/// (`results`, converted). A C callee is called as C takes its arguments.
/// Returns the function if this defined it, else null.
mlir::LLVM::LLVMFuncOp
defineForwarder(mlir::ConversionPatternRewriter &rewriter, mlir::Location loc,
                mlir::ModuleOp module, llvm::StringRef name,
                mlir::LLVM::LLVMFunctionType type,
                mlir::FlatSymbolRefAttr callee,
                llvm::ArrayRef<mlir::Type> results, unsigned firstOwn,
                llvm::function_ref<void(mlir::Block *,
                                        llvm::SmallVectorImpl<mlir::Value> &)>
                    leading) {
  if (module.lookupSymbol(name))
    return nullptr;
  mlir::OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToEnd(module.getBody());
  auto forwarder = mlir::LLVM::LLVMFuncOp::create(
      rewriter, loc, name, type, mlir::LLVM::Linkage::Internal);
  auto *entry = forwarder.addEntryBlock(rewriter);
  rewriter.setInsertionPointToStart(entry);
  llvm::SmallVector<mlir::Value, 4> args;
  leading(entry, args);
  for (auto arg : entry->getArguments().drop_front(firstOwn))
    args.push_back(arg);
  if (auto cFunc =
          module.lookupSymbol<mlir::LLVM::LLVMFuncOp>(callee.getValue())) {
    // A slot for C's struct passing, at the top of the forwarder.
    auto slot = [&](mlir::Type slotType) {
      return entrySlot(rewriter, *entry, loc, slotType);
    };
    auto value =
        cabi::callC(rewriter, loc, cFunc, args,
                    results.empty() ? mlir::Type() : results.front(), slot);
    mlir::LLVM::ReturnOp::create(
        rewriter, loc, value ? mlir::ValueRange{value} : mlir::ValueRange{});
  } else {
    auto call =
        mlir::func::CallOp::create(rewriter, loc, callee, results, args);
    mlir::LLVM::ReturnOp::create(rewriter, loc, call.getResults());
  }
  return forwarder;
}

/// C expects a char or bool argument or result already extended to 32 bits
/// by whoever produces it: signed for a char, unsigned for a bool.
std::optional<llvm::StringRef> cExtension(mlir::Type type) {
  if (type.isInteger(8))
    return mlir::LLVM::LLVMDialect::getSExtAttrName();
  if (type.isInteger(1))
    return mlir::LLVM::LLVMDialect::getZExtAttrName();
  return std::nullopt;
}

/// What C calls for the function value `fn`, passed to a C function: the
/// code of __c_<f>, which takes f's arguments as C passes them and calls f.
/// C holds no environment, so `fn` must name a function and bind nothing,
/// as the type checker sees to; and C passes only numbers, bool, char and
/// pointers to it, as the type checker sees to in the C declaration.
mlir::FailureOr<mlir::Value>
cFunctionPointer(mlir::ConversionPatternRewriter &rewriter,
                 const mlir::TypeConverter &converter, mlir::Value fn) {
  auto closure = fn.getDefiningOp<smn::ClosureOp>();
  if (!closure || !closure.getBound().empty())
    return mlir::failure();
  auto loc = closure.getLoc();
  auto *ctx = closure.getContext();
  auto signature = closure.getType().getSignature();
  llvm::SmallVector<mlir::Type> params, results;
  if (mlir::failed(converter.convertTypes(signature.getInputs(), params)) ||
      mlir::failed(converter.convertTypes(signature.getResults(), results)))
    return mlir::failure();
  auto type = mlir::LLVM::LLVMFunctionType::get(
      results.empty() ? mlir::LLVM::LLVMVoidType::get(ctx) : results.front(),
      params);
  auto name = ("__c_" + closure.getCallee()).str();
  auto module = closure->getParentOfType<mlir::ModuleOp>();
  if (auto thunk = defineForwarder(
          rewriter, loc, module, name, type, closure.getCalleeAttr(), results,
          /*firstOwn=*/0, [](mlir::Block *, auto &) {})) {
    for (auto [i, param] : llvm::enumerate(params))
      if (auto ext = cExtension(param))
        thunk.setArgAttr(static_cast<unsigned>(i), *ext,
                         mlir::UnitAttr::get(ctx));
    if (!results.empty())
      if (auto ext = cExtension(results.front()))
        thunk.setResultAttr(0, *ext, mlir::UnitAttr::get(ctx));
  }
  return mlir::LLVM::AddressOfOp::create(rewriter, loc, llvmPtr(ctx), name)
      .getResult();
}

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
      // A function C calls back takes a C function pointer.
      auto args = llvm::to_vector<4>(adaptor.getArgs());
      for (auto [i, arg] : llvm::enumerate(op.getArgs())) {
        if (!mlir::isa<smn::FnType>(arg.getType()))
          continue;
        auto pointer = cFunctionPointer(rewriter, *getTypeConverter(), arg);
        if (mlir::failed(pointer))
          return op.emitError("a function passed to C must name a function "
                              "and bind nothing");
        args[i] = *pointer;
      }
      auto value = cabi::callC(
          rewriter, op.getLoc(), cFunc, args,
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
  auto payload = fieldPtr(rewriter, loc, enumType, slot, 1);
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
    mlir::LLVM::StoreOp::create(rewriter, loc, tag,
                                fieldPtr(rewriter, loc, enumType, slot, 0));
    // A field of no size, such as a closure binding nothing, has no bytes
    // to write; an enum of only such has no payload at all.
    for (auto [field, mirField, offset] : llvm::zip(
             adaptor.getPayload(), op.getPayload().getTypes(), op.getOffsets()))
      if (smn::sizeOf(mirField) > 0)
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
    if (smn::sizeOf(op.getType()) == 0) {
      // Nothing to read: a value of no size (VariantLowering).
      rewriter.replaceOpWithNewOp<mlir::LLVM::PoisonOp>(op, fieldType);
      return mlir::success();
    }
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
    llvm::SmallVector<mlir::Type> wrapperParams{wordsParam(op.getType())};
    wrapperParams.append(rest);
    auto wrapperType = mlir::LLVM::LLVMFunctionType::get(
        results.empty() ? mlir::LLVM::LLVMVoidType::get(ctx) : results.front(),
        wrapperParams);

    auto bound = adaptor.getBound();
    auto closure = closureType(op.getType());
    mlir::LLVM::LLVMStructType envType;
    std::string name;
    if (bound.empty()) {
      name = ("__wrap_" + op.getCallee()).str();
    } else {
      envType = mlir::LLVM::LLVMStructType::getLiteral(
          ctx, llvm::to_vector(bound.getTypes()));
      do
        name = "__partial_" + std::to_string(partials++);
      while (module.lookupSymbol(name));
    }

    // Carried inline, what the closure binds is in the words the wrapper is
    // passed; held in the maker's frame, the words hold a pointer to it.
    bool inline_ = op.bindsInline();
    defineForwarder(
        rewriter, loc, module, name, wrapperType, op.getCalleeAttr(), results,
        /*firstOwn=*/1,
        [&](mlir::Block *entry, llvm::SmallVectorImpl<mlir::Value> &args) {
          // The words come by value; what they carry is read from a slot
          // of the wrapper's own, which LLVM's SROA takes apart.
          mlir::Value env;
          if (!bound.empty()) {
            auto words = entry->getArgument(0);
            env = entrySlot(rewriter, *entry, loc, words.getType());
            mlir::LLVM::StoreOp::create(rewriter, loc, words, env);
            if (!inline_)
              env = mlir::LLVM::LoadOp::create(rewriter, loc, ptrTy, env);
          }
          for (auto [i, value] : llvm::enumerate(bound))
            args.push_back(
                mlir::LLVM::LoadOp::create(rewriter, loc, value.getType(),
                                           fieldPtr(rewriter, loc, envType, env,
                                                    static_cast<int32_t>(i))));
        });

    // The closure carries what it binds in its words: built in a slot,
    // where they are written as the wrapper reads them, then loaded whole.
    auto code = mlir::LLVM::AddressOfOp::create(rewriter, loc, ptrTy, name);
    if (bound.empty()) {
      mlir::Value value = mlir::LLVM::PoisonOp::create(rewriter, loc, closure);
      value = mlir::LLVM::InsertValueOp::create(rewriter, loc, value, code,
                                                llvm::ArrayRef<int64_t>{0});
      rewriter.replaceOp(op, value);
      return mlir::success();
    }
    // ModuleGen sized the type by every closure of it; one it missed would
    // write past the closure's words.
    if (op.getType().getWords() == 0)
      return op.emitError("the compiler sized '")
             << op.getType() << "' without this closure, which binds values";
    auto slot = entrySlot(rewriter, op, closure);
    mlir::LLVM::StoreOp::create(rewriter, loc, code,
                                fieldPtr(rewriter, loc, closure, slot, 0));
    auto words = fieldPtr(rewriter, loc, closure, slot, 1);
    mlir::Value env = words;
    if (!inline_) {
      env = entrySlot(rewriter, op, envType);
      mlir::LLVM::StoreOp::create(rewriter, loc, env, words);
    }
    for (auto [i, value] : llvm::enumerate(bound))
      mlir::LLVM::StoreOp::create(
          rewriter, loc, value,
          fieldPtr(rewriter, loc, envType, env, static_cast<int32_t>(i)));
    rewriter.replaceOpWithNewOp<mlir::LLVM::LoadOp>(op, closure, slot);
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
    // The code is passed the closure's words, by value (wordsParam).
    auto fn = op.getCallee().getType();
    auto words = wordsParam(fn);
    mlir::Value env;
    if (fn.getWords() == 0)
      env = mlir::LLVM::ZeroOp::create(rewriter, loc, ptrTy);
    else
      env = mlir::LLVM::ExtractValueOp::create(rewriter, loc, words,
                                               adaptor.getCallee(),
                                               llvm::ArrayRef<int64_t>{1});
    llvm::SmallVector<mlir::Type> params{words};
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
    // A struct's drop is its Drop instance's call or its fields' drops.
    expandDrops(getOperation());

    mlir::RewritePatternSet patterns(&context);
    mlir::populateCallOpTypeConversionPattern(patterns, converter);
    mlir::populateReturnOpTypeConversionPattern(patterns, converter);
    mlir::populateFunctionOpInterfaceTypeConversionPattern<mlir::func::FuncOp>(
        patterns, converter);
    patterns.add<
        AllocLowering, DropLowering, LetLowering, ThroughLowering,
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
          return converter.isSignatureLegal(func.getFunctionType());
        });
    target.addLegalOp<mlir::UnrealizedConversionCastOp>();
    // The function's pattern converts only its entry block; each cf branch
    // converts the blocks it jumps to, e.g. an if's merge block.
    mlir::cf::populateCFStructuralTypeConversionsAndLegality(converter,
                                                             patterns, target);
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

mlir::Type llvmTypeOf(mlir::Type type) {
  return MIRTypeConverter().convertType(type);
}

std::unique_ptr<mlir::Pass> createMIRToLLVMPass() {
  return std::make_unique<MIRToLLVMPass>();
}

void registerMIRToLLVMPass() { mlir::PassRegistration<MIRToLLVMPass>(); }

} // namespace sammine_lang
