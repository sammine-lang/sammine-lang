#include "codegen/MIRLowering.h"
#include "codegen/ScalarOps.h"
#include "codegen/TypeLayout.h"
#include "typecheck/BuiltinInstances.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"

//! \file MIRLoweringOperators.cpp
//! \brief Arithmetic, negation, comparisons, equality and casts, the builtin
//! ones and those through instances.

namespace sammine_lang {

using namespace AST;

// `x as T`, through the Convert<A, T> instance the type checker chose.
mlir::FailureOr<mlir::Value> MIRLowering::lowerCast(CastExprAST *cast) {
  auto *cp = props_.cast(cast->id());
  if (!cp || !cp->instance)
    return mlir::failure();
  auto value = lowerValue(cast->operand.get());
  if (mlir::failed(value))
    return mlir::failure();
  auto &key = *cp->instance;
  if (is_builtin_instance(key))
    return lowerBuiltinInstance(loc(cast), key, *value, cast->get_type());
  auto converted =
      callMethod(loc(cast), key.to_typeclass_name(std::string(kConvertClass)),
                 *value, cast->get_type());
  if (mlir::succeeded(converted))
    converted->getDefiningOp()->setAttr(smn::kReasonAttr,
                                        builder_.getStringAttr("converted"));
  return converted;
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerNegation(UnaryNegExprAST *neg) {
  if (!neg->get_type().is_integer() && !neg->get_type().is_float())
    return mlir::failure();
  auto value = lowerValue(neg->operand.get());
  if (mlir::failed(value))
    return mlir::failure();
  return emitNegation(builder_, loc(neg), *value);
}

// An operator through the instance the type checker chose: one the program
// declares is a call to its method, one the compiler provides lowerOperator.
mlir::FailureOr<mlir::Value> MIRLowering::lowerBinary(BinaryExprAST *bin) {
  if (auto *bp = props_.binary(bin->id()); bp && bp->resolved_op_method) {
    auto lhs = lowerValue(bin->LHS.get());
    if (mlir::failed(lhs))
      return mlir::failure();
    auto rhs = lowerValue(bin->RHS.get());
    if (mlir::failed(rhs))
      return mlir::failure();
    return callMethod(loc(bin), *bp->resolved_op_method, {*lhs, *rhs},
                      bin->get_type());
  }
  auto lhs = lowerValue(bin->LHS.get());
  if (mlir::failed(lhs))
    return mlir::failure();
  auto rhs = lowerValue(bin->RHS.get());
  if (mlir::failed(rhs))
    return mlir::failure();
  // The operands' type decides what the operator does; a comparison's own
  // type is bool.
  return lowerOperator(loc(bin), bin->Op->tok_type, *lhs, *rhs,
                       bin->LHS->get_type());
}

// `lhs op rhs`, operands of `type`, through the instance the compiler
// provides. On integers, chars, bools, and an
// integer-backed enum's backing integer, and on floats, the operator itself;
// pointers compare their addresses, as signed integers; arrays and enums
// with payloads take == and != (lowerEquality).
mlir::FailureOr<mlir::Value>
MIRLowering::lowerOperator(mlir::Location location, TokenType op,
                           mlir::Value lhs, mlir::Value rhs, const Type &type) {
  bool comparison = isComparisonOp(op);
  mlir::Value result;
  if (type.type_kind == TypeKind::Pointer) {
    if (!comparison)
      return mlir::failure();
    auto address = [&](mlir::Value ptr) -> mlir::Value {
      return smn::AddressOp::create(builder_, location, builder_.getI64Type(),
                                    ptr);
    };
    result = emitScalarComparison(builder_, location, address(lhs),
                                  address(rhs), op, /*is_float=*/false,
                                  /*is_unsigned=*/false);
  } else if (auto integer = integerKind(type)) {
    result = comparison
                 ? emitScalarComparison(builder_, location, lhs, rhs, op,
                                        /*is_float=*/false, *integer)
                 : emitIntArithOp(builder_, location, lhs, rhs, op, *integer);
  } else if (type.is_float()) {
    result = comparison ? emitScalarComparison(builder_, location, lhs, rhs, op,
                                               /*is_float=*/true,
                                               /*is_unsigned=*/false)
                        : emitFloatArithOp(builder_, location, lhs, rhs, op);
  } else if (op == TokEQUAL || op == TokNOTEqual) {
    auto equal = lowerEquality(location, lhs, rhs, type);
    if (mlir::failed(equal) || op == TokEQUAL)
      return equal;
    auto one = intConstant(location, builder_.getI1Type(), 1);
    return mlir::arith::XOrIOp::create(builder_, location, *equal, one)
        .getResult();
  }
  if (!result)
    return mlir::failure();
  return result;
}

// Whether `type` is an integer to operators, and if so whether unsigned: an
// integer, a char (signed) or a bool, or an integer-backed enum, by its
// backing integer.
std::optional<bool> MIRLowering::integerKind(const Type &type) {
  if (type.is_integer())
    return type.is_unsigned();
  if (type.type_kind == TypeKind::Char || type.type_kind == TypeKind::Bool)
    return false;
  if (type.type_kind == TypeKind::Enum) {
    auto &et = std::get<EnumType>(type.type_data);
    if (et.is_integer_backed())
      return et.get_backing_type() == TypeKind::U32_t ||
             et.get_backing_type() == TypeKind::U64_t;
  }
  return std::nullopt;
}

// `lhs == rhs` of `type`, as an i1. Arrays compare element by element,
// stopping at the first that differs; enums with payloads compare their
// tags, then the payload of the variant they share.
mlir::FailureOr<mlir::Value> MIRLowering::lowerEquality(mlir::Location location,
                                                        mlir::Value lhs,
                                                        mlir::Value rhs,
                                                        const Type &type) {
  auto i1 = builder_.getI1Type();
  auto *region = builder_.getInsertionBlock()->getParent();

  if (type.type_kind == TypeKind::Array) {
    auto &array = std::get<ArrayType>(type.type_data);
    auto mirArray = mlir::dyn_cast<smn::ArrayType>(lhs.getType());
    if (!mirArray)
      return mlir::failure();
    auto i64 = builder_.getI64Type();
    auto *header = &region->emplaceBlock();
    header->addArgument(i64, location);
    auto *body = &region->emplaceBlock();
    auto *after = &region->emplaceBlock();
    after->addArgument(i1, location);
    mlir::cf::BranchOp::create(builder_, location, header,
                               mlir::ValueRange{intConstant(location, i64, 0)});
    builder_.setInsertionPointToStart(header);
    auto index = header->getArgument(0);
    auto more = mlir::arith::CmpIOp::create(
        builder_, location, mlir::arith::CmpIPredicate::slt, index,
        intConstant(location, i64, static_cast<int64_t>(array.get_size())));
    mlir::cf::CondBranchOp::create(
        builder_, location, more, body, mlir::ValueRange{}, after,
        mlir::ValueRange{intConstant(location, i1, 1)});
    builder_.setInsertionPointToStart(body);
    auto element = mirArray.getElement();
    auto a = smn::ElementOp::create(builder_, location, element, lhs, index);
    auto b = smn::ElementOp::create(builder_, location, element, rhs, index);
    auto same = lowerOperator(location, TokEQUAL, a, b, array.get_element());
    if (mlir::failed(same))
      return mlir::failure();
    auto next = mlir::arith::AddIOp::create(builder_, location, index,
                                            intConstant(location, i64, 1));
    mlir::cf::CondBranchOp::create(
        builder_, location, *same, header, mlir::ValueRange{next}, after,
        mlir::ValueRange{intConstant(location, i1, 0)});
    continueIn(after);
    return after->getArgument(0);
  }

  if (type.type_kind != TypeKind::Enum)
    return mlir::failure();
  auto et = std::get<EnumType>(type.type_data);
  auto enumType = mlir::dyn_cast<smn::EnumType>(lhs.getType());
  if (!enumType)
    return mlir::failure();
  auto i32 = builder_.getI32Type();
  auto lhsTag = smn::TagOp::create(builder_, location, i32, lhs);
  auto rhsTag = smn::TagOp::create(builder_, location, i32, rhs);
  auto sameTag = mlir::arith::CmpIOp::create(
      builder_, location, mlir::arith::CmpIPredicate::eq, lhsTag, rhsTag);
  auto *check = &region->emplaceBlock();
  auto *after = &region->emplaceBlock();
  after->addArgument(i1, location);
  mlir::cf::CondBranchOp::create(
      builder_, location, sameTag, check, mlir::ValueRange{}, after,
      mlir::ValueRange{intConstant(location, i1, 0)});
  // The same variant: with no payload, equal; else equal if its fields are.
  builder_.setInsertionPointToStart(check);
  auto equalTrue = intConstant(location, i1, 1);
  llvm::SmallVector<int32_t> tags;
  llvm::SmallVector<mlir::Block *> dests;
  for (auto [v, variant] : llvm::enumerate(et.get_variants()))
    if (!variant.payload_types.empty()) {
      tags.push_back(static_cast<int32_t>(v));
      dests.push_back(&region->emplaceBlock());
    }
  branchOn(location, lhsTag, tags, dests, after, mlir::ValueRange{equalTrue});
  for (auto [tag, dest] : llvm::zip(tags, dests)) {
    builder_.setInsertionPointToStart(dest);
    auto variant = static_cast<uint64_t>(tag);
    auto &fields = et.get_variant(variant).payload_types;
    auto mirFields = enumType.getPayload(variant);
    mlir::Value all = intConstant(location, i1, 1);
    int64_t offset = 0;
    for (auto [i, field] : llvm::enumerate(fields)) {
      auto at =
          static_cast<uint64_t>(layout::advancePayloadOffset(offset, field));
      auto a = smn::PayloadOp::create(builder_, location, mirFields[i], lhs,
                                      variant, i, at);
      auto b = smn::PayloadOp::create(builder_, location, mirFields[i], rhs,
                                      variant, i, at);
      auto same = lowerOperator(location, TokEQUAL, a, b, field);
      if (mlir::failed(same))
        return mlir::failure();
      all = mlir::arith::AndIOp::create(builder_, location, all, *same);
    }
    mlir::cf::BranchOp::create(builder_, location, after,
                               mlir::ValueRange{all});
  }
  continueIn(after);
  return after->getArgument(0);
}

} // namespace sammine_lang
