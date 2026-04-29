// Custom lowering: linalg.reduce on memrefs → gpu.launch + gpu.all_reduce.
//
// Upstream MLIR's linalg-to-parallel-loops only parallelizes parallel
// iterators, not reduction iterators. So linalg.reduce stays as scf.for,
// which never becomes a GPU kernel. This pass fills that gap by converting
// 1-D linalg.reduce ops directly to gpu.launch + gpu.all_reduce before
// linalg-to-parallel-loops runs.
//
// Approach matches what IREE and Triton do (custom reduction lowering),
// since upstream MLIR provides no built-in linalg.reduce → GPU path.

#include "codegen/LinalgReduceToGpu.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"

#include <optional>

namespace sammine_lang {

/// Inspect the body of a linalg.reduce to determine the gpu::AllReduceOperation.
static std::optional<mlir::gpu::AllReduceOperation>
detectReduceOp(mlir::linalg::ReduceOp reduceOp) {
  auto &body = reduceOp.getRegion();
  if (body.empty())
    return std::nullopt;

  auto &block = body.front();
  // After stablehlo-to-linalg + bufferize, the body may contain
  // memref.load/store wrappers around the core arith op. Scan all
  // ops to find the first recognized arith reduction op.
  mlir::Operation *arithOp = nullptr;
  for (auto &op : block.getOperations()) {
    if (llvm::isa<mlir::arith::AddFOp>(op) || llvm::isa<mlir::arith::AddIOp>(op) ||
        llvm::isa<mlir::arith::MulFOp>(op) || llvm::isa<mlir::arith::MulIOp>(op) ||
        llvm::isa<mlir::arith::MaximumFOp>(op) || llvm::isa<mlir::arith::MaxSIOp>(op) ||
        llvm::isa<mlir::arith::MinimumFOp>(op) || llvm::isa<mlir::arith::MinSIOp>(op)) {
      arithOp = &op;
      break;
    }
  }
  if (!arithOp)
    return std::nullopt;

  auto &op = *arithOp;
  using AROp = mlir::gpu::AllReduceOperation;

  if (llvm::isa<mlir::arith::AddFOp>(op) || llvm::isa<mlir::arith::AddIOp>(op))
    return AROp::ADD;
  if (llvm::isa<mlir::arith::MulFOp>(op) || llvm::isa<mlir::arith::MulIOp>(op))
    return AROp::MUL;
  if (llvm::isa<mlir::arith::MaximumFOp>(op))
    return AROp::MAXIMUMF;
  if (llvm::isa<mlir::arith::MaxSIOp>(op))
    return AROp::MAXSI;
  if (llvm::isa<mlir::arith::MinimumFOp>(op))
    return AROp::MINIMUMF;
  if (llvm::isa<mlir::arith::MinSIOp>(op))
    return AROp::MINSI;

  return std::nullopt;
}

mlir::LogicalResult lowerLinalgReduceToGpuLaunch(mlir::ModuleOp module) {
  llvm::SmallVector<mlir::linalg::ReduceOp> reduces;
  module->walk([&](mlir::linalg::ReduceOp op) { reduces.push_back(op); });

  if (reduces.empty())
    return mlir::success();

  for (auto reduceOp : reduces) {
    auto gpuReduceKind = detectReduceOp(reduceOp);
    if (!gpuReduceKind) {
      reduceOp.emitError("unsupported reduction op for GPU lowering");
      return mlir::failure();
    }

    auto dims = reduceOp.getDimensions();
    if (dims.size() != 1 || dims[0] != 0) {
      reduceOp.emitError("only 1-D reduction along dimension 0 supported");
      return mlir::failure();
    }

    auto input = reduceOp.getInputs()[0];
    auto output = reduceOp.getInits()[0];
    auto inputType = llvm::cast<mlir::MemRefType>(input.getType());
    auto loc = reduceOp.getLoc();

    if (inputType.getRank() != 1 || inputType.isDynamicDim(0)) {
      reduceOp.emitError("only static 1-D memrefs supported for GPU reduce");
      return mlir::failure();
    }
    int64_t arraySize = inputType.getDimSize(0);

    mlir::OpBuilder builder(reduceOp);
    auto c1 = mlir::arith::ConstantIndexOp::create(builder, loc, 1);
    auto cN = mlir::arith::ConstantIndexOp::create(builder, loc, arraySize);

    // gpu.launch blocks(1,1,1) threads(N,1,1)
    auto launchOp = mlir::gpu::LaunchOp::create(
        builder, loc,
        /*gridSizeX=*/c1, /*gridSizeY=*/c1, /*gridSizeZ=*/c1,
        /*blockSizeX=*/cN, /*blockSizeY=*/c1, /*blockSizeZ=*/c1);

    builder.setInsertionPointToStart(&launchOp.getBody().front());

    auto threadIdx = launchOp.getThreadIds().x;

    // Each thread loads its element
    auto elem = mlir::memref::LoadOp::create(builder, loc, input, threadIdx);

    // gpu.all_reduce: combine across all threads in the block.
    // All threads receive the same result, so any thread can store.
    auto opAttr = mlir::gpu::AllReduceOperationAttr::get(
        builder.getContext(), *gpuReduceKind);
    auto allReduce = mlir::gpu::AllReduceOp::create(
        builder, loc, elem, opAttr, /*uniform=*/nullptr);

    // Every thread stores the same value — redundant but avoids scf.if
    // inside gpu.launch (which requires extra lowering passes in gpu.module).
    mlir::memref::StoreOp::create(builder, loc, allReduce.getResult(), output,
                                   mlir::ValueRange{});
    mlir::gpu::TerminatorOp::create(builder, loc);

    reduceOp.erase();
  }

  return mlir::success();
}

} // namespace sammine_lang
