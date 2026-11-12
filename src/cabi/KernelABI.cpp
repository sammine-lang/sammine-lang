#include "cabi/KernelABI.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinTypes.h"

namespace sammine_lang::kernel_abi {

mlir::Value memrefOfPointer(mlir::OpBuilder &builder, mlir::Location loc,
                            mlir::Value ptr, int64_t size,
                            mlir::Type elemType) {
  // Build the LLVM struct that is the memref descriptor:
  //   { allocated_ptr, aligned_ptr, offset, sizes[1], strides[1] }
  auto i64Ty = builder.getI64Type();
  auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
  auto arrI64x1 = mlir::LLVM::LLVMArrayType::get(i64Ty, 1);
  auto descTy = mlir::LLVM::LLVMStructType::getLiteral(
      builder.getContext(), {ptrTy, ptrTy, i64Ty, arrI64x1, arrI64x1});

  mlir::Value desc = mlir::LLVM::UndefOp::create(builder, loc, descTy);
  desc = mlir::LLVM::InsertValueOp::create(builder, loc, desc, ptr,
                                           llvm::ArrayRef<int64_t>{0});
  desc = mlir::LLVM::InsertValueOp::create(builder, loc, desc, ptr,
                                           llvm::ArrayRef<int64_t>{1});
  auto zero = mlir::arith::ConstantIntOp::create(builder, loc, i64Ty, 0);
  desc = mlir::LLVM::InsertValueOp::create(builder, loc, desc, zero,
                                           llvm::ArrayRef<int64_t>{2});

  auto sizeVal = mlir::arith::ConstantIntOp::create(builder, loc, i64Ty, size);
  mlir::Value sizes = mlir::LLVM::UndefOp::create(builder, loc, arrI64x1);
  sizes = mlir::LLVM::InsertValueOp::create(builder, loc, sizes, sizeVal,
                                            llvm::ArrayRef<int64_t>{0});
  desc = mlir::LLVM::InsertValueOp::create(builder, loc, desc, sizes,
                                           llvm::ArrayRef<int64_t>{3});

  auto strideVal = mlir::arith::ConstantIntOp::create(builder, loc, i64Ty, 1);
  mlir::Value strides = mlir::LLVM::UndefOp::create(builder, loc, arrI64x1);
  strides = mlir::LLVM::InsertValueOp::create(builder, loc, strides, strideVal,
                                              llvm::ArrayRef<int64_t>{0});
  desc = mlir::LLVM::InsertValueOp::create(builder, loc, desc, strides,
                                           llvm::ArrayRef<int64_t>{4});

  // Cast LLVM descriptor struct → memref type
  auto memrefType = mlir::MemRefType::get({size}, elemType);
  return mlir::UnrealizedConversionCastOp::create(builder, loc, memrefType,
                                                  mlir::ValueRange{desc})
      .getResult(0);
}

} // namespace sammine_lang::kernel_abi
