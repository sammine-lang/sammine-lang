#include "codegen/MIRToLLVM.h"
#include "codegen/SammineDialect.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Transforms/FuncConversions.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Transforms/DialectConversion.h"

namespace sammine_lang {

namespace {

/// MIR's types as LLVM's: a pointer, owned or a view, is an !llvm.ptr. Any
/// other type is left as it is.
class MIRTypeConverter : public mlir::TypeConverter {
public:
  MIRTypeConverter() {
    addConversion([](mlir::Type type) { return type; });
    addConversion([](smn::PtrType type) -> mlir::Type {
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

struct MIRToLLVMPass
    : public mlir::PassWrapper<MIRToLLVMPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(MIRToLLVMPass)

  llvm::StringRef getArgument() const override { return "sammine-mir-to-llvm"; }
  llvm::StringRef getDescription() const override {
    return "Convert MIR's types and ops to the LLVM dialect";
  }
  void getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<mlir::LLVM::LLVMDialect>();
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

    mlir::ConversionTarget target(context);
    target.addDynamicallyLegalOp<mlir::func::FuncOp>(
        [&](mlir::func::FuncOp func) {
          return converter.isSignatureLegal(func.getFunctionType()) &&
                 converter.isLegal(&func.getBody());
        });
    target.addLegalOp<mlir::UnrealizedConversionCastOp>();
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
