#include "dialect/MIRToLLVM.h"
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

/// MIR's types as LLVM's: a pointer, owned or a view, and a place are each an
/// !llvm.ptr. Any other type is left as it is.
class MIRTypeConverter : public mlir::TypeConverter {
public:
  MIRTypeConverter() {
    addConversion([](mlir::Type type) { return type; });
    addConversion([](smn::PtrType type) -> mlir::Type {
      return mlir::LLVM::LLVMPointerType::get(type.getContext());
    });
    addConversion([](smn::PlaceType type) -> mlir::Type {
      return mlir::LLVM::LLVMPointerType::get(type.getContext());
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

// alloc<T>(n): malloc(sizeof(T) * n), n widened to i64 as the AST codegen
// does.
struct AllocLowering : public mlir::OpConversionPattern<smn::AllocOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::AllocOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto ptrTy = mlir::LLVM::LLVMPointerType::get(op.getContext());
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
    auto sizeVal = mlir::arith::ConstantIntOp::create(rewriter, loc, i64Ty, size);
    auto bytes = mlir::arith::MulIOp::create(rewriter, loc, sizeVal, count);

    declareCFunction(op->getParentOfType<mlir::ModuleOp>(), rewriter, "malloc",
                     mlir::LLVM::LLVMFunctionType::get(ptrTy, {i64Ty}));
    auto call = mlir::LLVM::CallOp::create(rewriter, loc,
                                           mlir::TypeRange{ptrTy}, "malloc",
                                           mlir::ValueRange{bytes});
    rewriter.replaceOp(op, call.getResult());
    return mlir::success();
  }
};

struct FreeLowering : public mlir::OpConversionPattern<smn::FreeOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::FreeOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto ptrTy = mlir::LLVM::LLVMPointerType::get(op.getContext());
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

// A place is where its pointer points: *p is p itself.
struct DerefLowering : public mlir::OpConversionPattern<smn::DerefOp> {
  using OpConversionPattern::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(smn::DerefOp op, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    rewriter.replaceOp(op, adaptor.getPtr());
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
    mlir::populateFunctionOpInterfaceTypeConversionPattern<mlir::func::FuncOp>(
        patterns, converter);
    mlir::populateCallOpTypeConversionPattern(patterns, converter);
    mlir::populateReturnOpTypeConversionPattern(patterns, converter);
    mlir::populateBranchOpInterfaceTypeConversionPattern(patterns, converter);
    patterns.add<AllocLowering, FreeLowering, DerefLowering,
                 ReadLowering<smn::CopyOp>, ReadLowering<smn::MoveOp>,
                 StoreLowering>(converter, &context);

    mlir::ConversionTarget target(context);
    target.addDynamicallyLegalOp<mlir::func::FuncOp>(
        [&](mlir::func::FuncOp func) {
          return converter.isSignatureLegal(func.getFunctionType()) &&
                 converter.isLegal(&func.getBody());
        });
    target.addLegalOp<mlir::UnrealizedConversionCastOp>();
    target.addIllegalOp<smn::AllocOp, smn::FreeOp, smn::DerefOp, smn::CopyOp,
                        smn::MoveOp, smn::StoreOp>();
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
