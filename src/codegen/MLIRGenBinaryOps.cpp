#include "codegen/MLIRGenBinaryOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

namespace sammine_lang {

mlir::Value emitIntArithOp(mlir::OpBuilder &builder, mlir::Location location,
                           mlir::Value lhs, mlir::Value rhs, TokenType op,
                           bool is_unsigned) {
  switch (op) {
  case TokADD:
    return mlir::arith::AddIOp::create(builder, location, lhs, rhs).getResult();
  case TokSUB:
    return mlir::arith::SubIOp::create(builder, location, lhs, rhs).getResult();
  case TokMUL:
    return mlir::arith::MulIOp::create(builder, location, lhs, rhs).getResult();
  case TokDIV:
    if (is_unsigned)
      return mlir::arith::DivUIOp::create(builder, location, lhs, rhs)
          .getResult();
    return mlir::arith::DivSIOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokMOD:
    if (is_unsigned)
      return mlir::arith::RemUIOp::create(builder, location, lhs, rhs)
          .getResult();
    return mlir::arith::RemSIOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokAndLogical:
    return mlir::arith::AndIOp::create(builder, location, lhs, rhs).getResult();
  case TokORLogical:
    return mlir::arith::OrIOp::create(builder, location, lhs, rhs).getResult();
  case TokXOR:
    return mlir::arith::XOrIOp::create(builder, location, lhs, rhs).getResult();
  case TokSHL:
    return mlir::arith::ShLIOp::create(builder, location, lhs, rhs).getResult();
  case TokSHR:
    if (is_unsigned)
      return mlir::arith::ShRUIOp::create(builder, location, lhs, rhs)
          .getResult();
    return mlir::arith::ShRSIOp::create(builder, location, lhs, rhs)
        .getResult();
  default:
    return nullptr;
  }
}

mlir::Value emitFloatArithOp(mlir::OpBuilder &builder, mlir::Location location,
                             mlir::Value lhs, mlir::Value rhs, TokenType op) {
  switch (op) {
  case TokADD:
    return mlir::arith::AddFOp::create(builder, location, lhs, rhs).getResult();
  case TokSUB:
    return mlir::arith::SubFOp::create(builder, location, lhs, rhs).getResult();
  case TokMUL:
    return mlir::arith::MulFOp::create(builder, location, lhs, rhs).getResult();
  case TokDIV:
    return mlir::arith::DivFOp::create(builder, location, lhs, rhs).getResult();
  case TokMOD:
    return mlir::arith::RemFOp::create(builder, location, lhs, rhs).getResult();
  default:
    return nullptr;
  }
}

mlir::Value emitNumberConversion(mlir::OpBuilder &builder,
                                 mlir::Location location, mlir::Value value,
                                 mlir::Type to, bool from_unsigned,
                                 bool to_unsigned) {
  auto from = value.getType();
  if (from == to)
    return value;

  auto from_int = mlir::dyn_cast<mlir::IntegerType>(from);
  auto to_int = mlir::dyn_cast<mlir::IntegerType>(to);
  if (from_int && to_int) {
    if (from_int.getWidth() > to_int.getWidth())
      return mlir::arith::TruncIOp::create(builder, location, to, value);
    if (from_unsigned)
      return mlir::arith::ExtUIOp::create(builder, location, to, value);
    return mlir::arith::ExtSIOp::create(builder, location, to, value);
  }
  if (from_int) {
    if (from_unsigned)
      return mlir::arith::UIToFPOp::create(builder, location, to, value);
    return mlir::arith::SIToFPOp::create(builder, location, to, value);
  }
  // Saturate, so every float has a defined result: NaN becomes 0 and a value
  // out of range becomes the type's min or max. Plain fptosi gives poison.
  if (to_int)
    return mlir::LLVM::CallIntrinsicOp::create(
               builder, location, to,
               builder.getStringAttr(to_unsigned ? "llvm.fptoui.sat"
                                                 : "llvm.fptosi.sat"),
               mlir::ValueRange{value})
        .getResult(0);
  if (mlir::cast<mlir::FloatType>(from).getWidth() >
      mlir::cast<mlir::FloatType>(to).getWidth())
    return mlir::arith::TruncFOp::create(builder, location, to, value);
  return mlir::arith::ExtFOp::create(builder, location, to, value);
}

} // namespace sammine_lang
