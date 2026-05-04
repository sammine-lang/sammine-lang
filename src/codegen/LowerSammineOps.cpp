#include "codegen/LowerSammineOps.h"
#include "codegen/SammineDialect.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

namespace sammine_lang {
namespace {

struct LowerToDevice : public mlir::OpRewritePattern<smn::ToDeviceOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(smn::ToDeviceOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto memrefType = mlir::cast<mlir::MemRefType>(op.getInput().getType());
    auto tokenType = mlir::gpu::AsyncTokenType::get(rewriter.getContext());

    auto waitOp = mlir::gpu::WaitOp::create(rewriter, loc, tokenType,
                                             mlir::ValueRange{});
    auto allocOp = mlir::gpu::AllocOp::create(
        rewriter, loc, memrefType, tokenType,
        mlir::ValueRange{waitOp.getAsyncToken()}, mlir::ValueRange{},
        mlir::ValueRange{});
    auto memcpyOp = mlir::gpu::MemcpyOp::create(
        rewriter, loc, tokenType,
        mlir::ValueRange{allocOp.getAsyncToken()}, allocOp.getMemref(),
        op.getInput());
    mlir::gpu::WaitOp::create(rewriter, loc, mlir::Type{},
                              mlir::ValueRange{memcpyOp.getAsyncToken()});

    rewriter.replaceOp(op, allocOp.getMemref());
    return mlir::success();
  }
};

struct LowerToHost : public mlir::OpRewritePattern<smn::ToHostOp> {
  using OpRewritePattern::OpRewritePattern;

  mlir::LogicalResult
  matchAndRewrite(smn::ToHostOp op,
                  mlir::PatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    auto memrefType = mlir::cast<mlir::MemRefType>(op.getInput().getType());
    auto tokenType = mlir::gpu::AsyncTokenType::get(rewriter.getContext());

    // Find the sret output buffer (last memref block arg of the enclosing func).
    auto funcOp = op->getParentOfType<mlir::func::FuncOp>();
    auto &entryBlock = funcOp.getBody().front();
    mlir::Value hostDst;
    for (int i = static_cast<int>(entryBlock.getNumArguments()) - 1; i >= 0;
         --i) {
      if (entryBlock.getArgument(static_cast<unsigned>(i)).getType() ==
          memrefType) {
        hostDst = entryBlock.getArgument(static_cast<unsigned>(i));
        break;
      }
    }
    if (!hostDst) {
      return op.emitError("could not find host destination memref");
    }

    auto waitOp = mlir::gpu::WaitOp::create(rewriter, loc, tokenType,
                                             mlir::ValueRange{});
    auto memcpyOp = mlir::gpu::MemcpyOp::create(
        rewriter, loc, tokenType, mlir::ValueRange{waitOp.getAsyncToken()},
        hostDst, op.getInput());
    auto deallocOp = mlir::gpu::DeallocOp::create(
        rewriter, loc, tokenType,
        mlir::ValueRange{memcpyOp.getAsyncToken()}, op.getInput());
    mlir::gpu::WaitOp::create(rewriter, loc, mlir::Type{},
                              mlir::ValueRange{deallocOp.getAsyncToken()});

    rewriter.replaceOp(op, hostDst);
    return mlir::success();
  }
};

struct LowerSammineOpsPass
    : public mlir::PassWrapper<LowerSammineOpsPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerSammineOpsPass)

  void getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<mlir::gpu::GPUDialect>();
    registry.insert<mlir::memref::MemRefDialect>();
  }

  void runOnOperation() override {
    auto module = getOperation();
    mlir::RewritePatternSet patterns(&getContext());
    patterns.add<LowerToDevice, LowerToHost>(&getContext());

    mlir::ConversionTarget target(getContext());
    target.addIllegalOp<smn::ToDeviceOp, smn::ToHostOp>();
    target.markUnknownOpDynamicallyLegal([](mlir::Operation *) { return true; });

    if (mlir::failed(
            mlir::applyPartialConversion(module, target, std::move(patterns))))
      signalPassFailure();
  }

  llvm::StringRef getArgument() const final { return "lower-sammine-ops"; }
  llvm::StringRef getDescription() const final {
    return "Lower sammine.to_device/to_host to GPU dialect ops";
  }
};

} // namespace

std::unique_ptr<mlir::Pass> createLowerSammineOpsPass() {
  return std::make_unique<LowerSammineOpsPass>();
}

} // namespace sammine_lang
