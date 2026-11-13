#include "codegen/MIRLowering.h"
#include "codegen/Runtime.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/IR/Matchers.h"

//! \file MIRLoweringAggregates.cpp
//! \brief Struct, tuple, array and enum values: literals, fields, `len`, `dim`,
//! ranges and checked `a[i]`.

namespace sammine_lang {

using namespace AST;

// `Option<i64>::Some(x)`, or a variant with no payload, `None`: its payload
// fields at the offsets smn::payloadOffsets gives them.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerVariant(mlir::Location location, const Type &type,
                          size_t index, llvm::ArrayRef<ExprAST *> payload) {
  auto mir = types_.mirType(type);
  if (!mir)
    return mlir::failure();
  // A variant of an integer-backed enum is its discriminant.
  if (auto backing = mlir::dyn_cast<mlir::IntegerType>(*mir)) {
    auto discriminant = std::get<EnumType>(type.type_data)
                            .get_variant(index)
                            .discriminant_value;
    if (!discriminant || !payload.empty())
      return mlir::failure();
    return intConstant(location, backing, *discriminant);
  }
  auto enumType = mlir::cast<smn::EnumType>(*mir);
  auto &payloadTypes =
      std::get<EnumType>(type.type_data).get_variant(index).payload_types;
  if (payload.size() != payloadTypes.size())
    return mlir::failure();
  llvm::SmallVector<mlir::Value> fields;
  for (auto [expr, mirField] : llvm::zip(payload, enumType.getPayload(index))) {
    auto value = lowerValue(expr);
    if (mlir::failed(value) || value->getType() != mirField)
      return mlir::failure();
    fields.push_back(*value);
  }
  auto offsets = smn::payloadOffsets(enumType.getPayload(index));
  return smn::VariantOp::create(builder_, location, enumType, index, offsets,
                                fields)
      .getResult();
}

// The values of `exprs`, in order, as the fields of an aggregate of `type`.
mlir::FailureOr<mlir::Value> MIRLowering::lowerAggregate(
    mlir::Location location, mlir::Type type,
    llvm::ArrayRef<mlir::Type> fieldTypes,
    llvm::ArrayRef<std::pair<size_t, ExprAST *>> exprs) {
  llvm::SmallVector<mlir::Value> fields(fieldTypes.size());
  for (auto [index, expr] : exprs) {
    auto value = lowerValue(expr);
    if (mlir::failed(value) || value->getType() != fieldTypes[index])
      return mlir::failure();
    fields[index] = *value;
  }
  if (llvm::is_contained(fields, mlir::Value()))
    return mlir::failure();
  return smn::AggregateOp::create(builder_, location, type, fields).getResult();
}

// `Point { y: b, x: a }`: the fields' values in the order written, each put
// at its field's place.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerStructLiteral(StructLiteralExprAST *lit) {
  auto type = types_.mirType(lit->get_type());
  if (!type)
    return mlir::failure();
  auto st = std::get<StructType>(lit->get_type().type_data);
  llvm::SmallVector<std::pair<size_t, ExprAST *>> exprs;
  for (auto [name, value] : llvm::zip(lit->field_names, lit->field_values)) {
    auto index = st.get_field_index(name);
    if (!index)
      return mlir::failure();
    exprs.push_back({*index, value.get()});
  }
  return lowerAggregate(loc(lit), *type,
                        mlir::cast<smn::StructType>(*type).getFields(), exprs);
}

mlir::FailureOr<mlir::Value>
MIRLowering::lowerTupleLiteral(TupleLiteralExprAST *tuple) {
  auto type = types_.mirType(tuple->get_type());
  if (!type)
    return mlir::failure();
  llvm::SmallVector<std::pair<size_t, ExprAST *>> exprs;
  for (auto [i, element] : llvm::enumerate(tuple->elements))
    exprs.push_back({i, element.get()});
  return lowerAggregate(loc(tuple), *type,
                        mlir::cast<smn::TupleType>(*type).getElements(), exprs);
}

// `p.x`, or `t.0` of a tuple: read from its place when it has one, so only
// the field is copied or moved, else taken from the aggregate's value.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerFieldAccess(FieldAccessExprAST *access) {
  if (hasPlace(access))
    return lowerPlaceRead(access);
  auto index = fieldIndex(access);
  auto type = types_.mirType(access->get_type());
  if (!index || !type)
    return mlir::failure();
  auto object = lowerValue(access->object_expr.get());
  if (mlir::failed(object))
    return mlir::failure();
  return smn::ExtractOp::create(builder_, loc(access), *type, *object, *index)
      .getResult();
}

// `[a, b, c]`: its elements' values, in order.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerArrayLiteral(ArrayLiteralExprAST *array) {
  auto type = types_.mirType(array->get_type());
  if (!type)
    return mlir::failure();
  auto arrayType = mlir::cast<smn::ArrayType>(*type);
  if (static_cast<size_t>(arrayType.getSize()) != array->elements.size())
    return mlir::failure();
  llvm::SmallVector<mlir::Type> elementTypes(array->elements.size(),
                                             arrayType.getElement());
  llvm::SmallVector<std::pair<size_t, ExprAST *>> exprs;
  for (auto [i, element] : llvm::enumerate(array->elements))
    exprs.push_back({i, element.get()});
  return lowerAggregate(loc(array), *type, elementTypes, exprs);
}

// `[a...b]`, an array of a, a + 1, ..., b: the type checker has a and b
// constants, so it is the array literal of those constants.
mlir::FailureOr<mlir::Value> MIRLowering::lowerRangeArray(RangeExprAST *range) {
  auto type = types_.mirType(range->get_type());
  if (!type)
    return mlir::failure();
  auto array = mlir::cast<smn::ArrayType>(*type);
  auto element = array.getElement();
  auto start = lowerValue(range->start.get());
  llvm::APInt first;
  if (mlir::failed(start) || !mlir::matchPattern(*start, mlir::m_ConstantInt(&first)))
    return mlir::failure();
  auto location = loc(range);
  llvm::SmallVector<mlir::Value> elements;
  for (int64_t i = 0; i < array.getSize(); ++i)
    elements.push_back(
        intConstant(location, element, first.getSExtValue() + i));
  return smn::AggregateOp::create(builder_, location, array, elements)
      .getResult();
}

// An array's length is in its type.
mlir::FailureOr<mlir::Value> MIRLowering::lowerLen(LenExprAST *len) {
  auto operand = len->operand->get_type();
  if (operand.type_kind != TypeKind::Array)
    return mlir::failure();
  auto size = std::get<ArrayType>(operand.type_data).get_size();
  return intConstant(loc(len), builder_.getI32Type(),
                     static_cast<int64_t>(size));
}

// `dim(a)`: the sizes of a nested array's dimensions, a tuple of i32
// constants, from its type alone.
mlir::FailureOr<mlir::Value> MIRLowering::lowerDim(DimExprAST *dim) {
  auto type = types_.mirType(dim->get_type());
  if (!type)
    return mlir::failure();
  llvm::SmallVector<mlir::Value> sizes;
  for (Type current = dim->operand->get_type();
       current.type_kind == TypeKind::Array;
       current = std::get<ArrayType>(current.type_data).get_element())
    sizes.push_back(
        intConstant(loc(dim), builder_.getI32Type(),
                    static_cast<int64_t>(
                        std::get<ArrayType>(current.type_data).get_size())));
  return smn::AggregateOp::create(builder_, loc(dim), *type, sizes).getResult();
}

// `a[i]` of an array: i is checked first, and one out of bounds reported on
// stderr before the program exits.
mlir::FailureOr<mlir::Value> MIRLowering::lowerArrayIndex(IndexExprAST *index) {
  auto array = lowerValue(index->array_expr.get());
  if (mlir::failed(array))
    return mlir::failure();
  auto offset = lowerValue(index->index_expr.get());
  if (mlir::failed(offset))
    return mlir::failure();
  auto arrayType = mlir::dyn_cast<smn::ArrayType>(array->getType());
  if (!arrayType || !mlir::isa<mlir::IntegerType>(offset->getType()))
    return mlir::failure();
  if (mlir::failed(checkBounds(loc(index), *offset, arrayType.getSize())))
    return mlir::failure();
  return smn::ElementOp::create(builder_, loc(index), arrayType.getElement(),
                                *array, *offset)
      .getResult();
}

// Whether `offset` is within an array of `size` elements, compared as i64 so
// that a wide index can't wrap into range. Out of it, the index and the
// size go to stderr through dprintf, then exit(1); in it, lowering goes on
// in a block of its own.
mlir::LogicalResult MIRLowering::checkBounds(mlir::Location location,
                                             mlir::Value offset, int64_t size) {
  auto i64 = builder_.getI64Type();
  mlir::Value wide = offset;
  if (offset.getType() != i64)
    wide = mlir::arith::ExtSIOp::create(builder_, location, i64, offset);
  auto sizeValue = intConstant(location, i64, size);
  auto zero = intConstant(location, i64, 0);
  auto high = mlir::arith::CmpIOp::create(
      builder_, location, mlir::arith::CmpIPredicate::sge, wide, sizeValue);
  auto low = mlir::arith::CmpIOp::create(
      builder_, location, mlir::arith::CmpIPredicate::slt, wide, zero);
  auto outside = mlir::arith::OrIOp::create(builder_, location, high, low);

  auto *region = builder_.getInsertionBlock()->getParent();
  auto *outOfBounds = &region->emplaceBlock();
  auto *inside = &region->emplaceBlock();
  mlir::cf::CondBranchOp::create(builder_, location, outside, outOfBounds,
                                 inside);

  builder_.setInsertionPointToStart(outOfBounds);
  if (mlir::failed(fail(
          location, "index %lld out of bounds for an array of length %lld\n",
          mlir::ValueRange{wide, sizeValue})))
    return mlir::failure();

  builder_.setInsertionPointToStart(inside);
  return mlir::success();
}

mlir::LogicalResult MIRLowering::fail(mlir::Location location,
                                      llvm::StringRef format,
                                      mlir::ValueRange args) {
  if (!module_.lookupSymbol(runtime::kDprintf) ||
      !module_.lookupSymbol(runtime::kExit))
    return mlir::failure();
  auto i32 = builder_.getI32Type();
  auto message = smn::StringOp::create(builder_, location,
                                       viewOf(builder_.getI8Type()), format);
  llvm::SmallVector<mlir::Value> operands{intConstant(location, i32, 2),
                                          message};
  operands.append(args.begin(), args.end());
  smn::CallOp::create(builder_, location, mlir::TypeRange{i32},
                      runtime::kDprintf, operands);
  smn::CallOp::create(builder_, location, mlir::TypeRange{}, runtime::kExit,
                      mlir::ValueRange{intConstant(location, i32, 1)});
  smn::UnreachableOp::create(builder_, location);
  return mlir::success();
}

// panic(message, args...): `file:line:col: panic: ` and the message, with
// a newline, is dprintf's format, and the arguments what it formats. The
// block ends there, unreachable, so no drop is put after it and no path
// through it joins the rest: its value is none.
mlir::FailureOr<mlir::Value> MIRLowering::lowerPanic(PanicExprAST *panic) {
  auto location = loc(panic);
  auto *message = llvm::dyn_cast<StringExprAST>(panic->arguments.front().get());
  if (!message)
    return mlir::failure();
  std::string format;
  if (auto range = mlir::dyn_cast<mlir::FileLineColRange>(location)) {
    // The file's name is printed as it is: a `%` in it isn't a format.
    std::string where;
    for (char c : range.getFilename().getValue())
      where += c == '%' ? std::string("%%") : std::string(1, c);
    format = where + ":" + std::to_string(range.getStartLine()) + ":" +
             std::to_string(range.getStartColumn() + 1) + ": ";
  }
  format += "panic: " + message->string_content + "\n";
  llvm::SmallVector<mlir::Value> args;
  for (auto &arg : llvm::drop_begin(panic->arguments)) {
    auto value = lowerValue(arg.get());
    if (mlir::failed(value))
      return mlir::failure();
    args.push_back(*value);
  }
  if (mlir::failed(fail(location, format, args)))
    return mlir::failure();
  return mlir::Value();
}

/// Which field of its struct or tuple `access` reads.
std::optional<size_t>
MIRLowering::fieldIndex(FieldAccessExprAST *access) const {
  auto objectType = access->object_expr->get_type();
  if (objectType.type_kind == TypeKind::Tuple)
    return std::stoul(access->field_name);
  if (objectType.type_kind == TypeKind::Struct)
    return std::get<StructType>(objectType.type_data)
        .get_field_index(access->field_name);
  return std::nullopt;
}

} // namespace sammine_lang
