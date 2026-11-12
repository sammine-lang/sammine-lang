#include "codegen/MLIRGenBinaryOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

#include <unordered_map>

namespace sammine_lang {

// Each comparison's predicate for signed and unsigned integers and for
// floats.
struct CmpPredicates {
  mlir::arith::CmpIPredicate signed_int;
  mlir::arith::CmpIPredicate unsigned_int;
  mlir::arith::CmpFPredicate flt;
};

static const CmpPredicates *lookupCmpPredicate(TokenType tok) {
  static const std::unordered_map<TokenType, CmpPredicates> table = {
      {TokEQUAL,
       {mlir::arith::CmpIPredicate::eq, mlir::arith::CmpIPredicate::eq,
        mlir::arith::CmpFPredicate::OEQ}},
      {TokNOTEqual,
       {mlir::arith::CmpIPredicate::ne, mlir::arith::CmpIPredicate::ne,
        mlir::arith::CmpFPredicate::ONE}},
      {TokLESS,
       {mlir::arith::CmpIPredicate::slt, mlir::arith::CmpIPredicate::ult,
        mlir::arith::CmpFPredicate::OLT}},
      {TokLessEqual,
       {mlir::arith::CmpIPredicate::sle, mlir::arith::CmpIPredicate::ule,
        mlir::arith::CmpFPredicate::OLE}},
      {TokGREATER,
       {mlir::arith::CmpIPredicate::sgt, mlir::arith::CmpIPredicate::ugt,
        mlir::arith::CmpFPredicate::OGT}},
      {TokGreaterEqual,
       {mlir::arith::CmpIPredicate::sge, mlir::arith::CmpIPredicate::uge,
        mlir::arith::CmpFPredicate::OGE}},
  };
  auto it = table.find(tok);
  return it != table.end() ? &it->second : nullptr;
}

bool isComparisonOp(TokenType op) { return lookupCmpPredicate(op) != nullptr; }

mlir::Value emitScalarComparison(mlir::OpBuilder &builder,
                                 mlir::Location location, mlir::Value lhs,
                                 mlir::Value rhs, TokenType op, bool is_float,
                                 bool is_unsigned) {
  auto *cmp = lookupCmpPredicate(op);
  if (!cmp)
    return nullptr;
  if (is_float)
    return mlir::arith::CmpFOp::create(builder, location, cmp->flt, lhs, rhs)
        .getResult();
  return mlir::arith::CmpIOp::create(
             builder, location,
             is_unsigned ? cmp->unsigned_int : cmp->signed_int, lhs, rhs)
      .getResult();
}

mlir::Value emitFloatConstant(mlir::OpBuilder &builder, mlir::Location location,
                              double value, mlir::FloatType type) {
  llvm::APFloat apVal(value);
  if (type.isF32()) {
    bool losesInfo = false;
    apVal.convert(llvm::APFloat::IEEEsingle(),
                  llvm::APFloat::rmNearestTiesToEven, &losesInfo);
  }
  return mlir::arith::ConstantFloatOp::create(builder, location, type, apVal)
      .getResult();
}

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
