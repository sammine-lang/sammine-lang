#include "codegen/MLIRGenBinaryOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

namespace sammine_lang {

mlir::Value emitIntArithOp(mlir::OpBuilder &builder, mlir::Location location,
                           mlir::Value lhs, mlir::Value rhs, TokenType op,
                           bool is_unsigned) {
  switch (op) {
  case TokADD:
    return mlir::arith::AddIOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokSUB:
    return mlir::arith::SubIOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokMUL:
    return mlir::arith::MulIOp::create(builder, location, lhs, rhs)
        .getResult();
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
    return mlir::arith::AndIOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokORLogical:
    return mlir::arith::OrIOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokXOR:
    return mlir::arith::XOrIOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokSHL:
    return mlir::arith::ShLIOp::create(builder, location, lhs, rhs)
        .getResult();
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
    return mlir::arith::AddFOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokSUB:
    return mlir::arith::SubFOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokMUL:
    return mlir::arith::MulFOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokDIV:
    return mlir::arith::DivFOp::create(builder, location, lhs, rhs)
        .getResult();
  case TokMOD:
    return mlir::arith::RemFOp::create(builder, location, lhs, rhs)
        .getResult();
  default:
    return nullptr;
  }
}

} // namespace sammine_lang
