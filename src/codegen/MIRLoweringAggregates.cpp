#include "codegen/MIRLowering.h"
#include "codegen/Runtime.h"
#include "typecheck/BuiltinInstances.h"
#include "util/FormatString.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
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

// A Formatter is one word. Its lowest bit says where: 0 stdout, 1 stderr.
// Its upper half is a float's digits after the point, plus one, or 0 for
// none said.
static constexpr int64_t kToStderr = 1;
static constexpr int64_t kPrecisionShift = 32;

static int64_t formatterBits(bool toStderr, std::optional<unsigned> precision) {
  return (toStderr ? kToStderr : 0) |
         (precision ? (static_cast<int64_t>(*precision) + 1) << kPrecisionShift
                    : 0);
}

// The Formatter a method is handed for a `{}` written to `to`, with the
// placeholder's digits.
mlir::Value MIRLowering::formatterFor(mlir::Location location,
                                      const WriteTo &to,
                                      std::optional<unsigned> precision) {
  auto i64 = builder_.getI64Type();
  if (!to.formatter)
    return intConstant(location, i64, formatterBits(to.stderr_, precision));
  // write(f, ...): where f writes, and this placeholder's own digits.
  auto where = mlir::arith::AndIOp::create(
      builder_, location, to.formatter, intConstant(location, i64, kToStderr));
  if (!precision)
    return where;
  return mlir::arith::OrIOp::create(
      builder_, location, where,
      intConstant(location, i64, formatterBits(false, precision)));
}

// Writes `format`, C's, with `args`, where `to` says: stdout through
// printf, whose buffer C's own output shares, and stderr, which has none,
// through dprintf.
void MIRLowering::write(mlir::Location location, const WriteTo &to,
                        llvm::StringRef format, mlir::ValueRange args) {
  auto i32 = builder_.getI32Type();
  auto text = smn::StringOp::create(builder_, location,
                                    viewOf(builder_.getI8Type()), format)
                  .getResult();
  auto toStdout = [&] {
    llvm::SmallVector<mlir::Value> operands{text};
    operands.append(args.begin(), args.end());
    smn::CallOp::create(builder_, location, mlir::TypeRange{i32},
                        runtime::kPrintf, operands);
  };
  auto toStderr = [&] {
    llvm::SmallVector<mlir::Value> operands{intConstant(location, i32, 2),
                                            text};
    operands.append(args.begin(), args.end());
    smn::CallOp::create(builder_, location, mlir::TypeRange{i32},
                        runtime::kDprintf, operands);
  };
  // Which it is, is known where the program wrote print, eprint or panic.
  if (!to.formatter)
    return to.stderr_ ? toStderr() : toStdout();
  auto i64 = builder_.getI64Type();
  auto where = mlir::arith::AndIOp::create(
      builder_, location, to.formatter, intConstant(location, i64, kToStderr));
  auto isStderr = mlir::arith::CmpIOp::create(
      builder_, location, mlir::arith::CmpIPredicate::ne, where,
      intConstant(location, i64, 0));
  auto *region = builder_.getInsertionBlock()->getParent();
  auto *err = &region->emplaceBlock();
  auto *out = &region->emplaceBlock();
  auto *after = &region->emplaceBlock();
  mlir::cf::CondBranchOp::create(builder_, location, isStderr, err, out);
  builder_.setInsertionPointToStart(err);
  toStderr();
  mlir::cf::BranchOp::create(builder_, location, after);
  builder_.setInsertionPointToStart(out);
  toStdout();
  mlir::cf::BranchOp::create(builder_, location, after);
  builder_.setInsertionPointToStart(after);
}

mlir::LogicalResult MIRLowering::fail(mlir::Location location,
                                      llvm::StringRef format,
                                      mlir::ValueRange args) {
  auto i32 = builder_.getI32Type();
  write(location, WriteTo{/*stderr_=*/true, {}}, format, args);
  smn::CallOp::create(builder_, location, mlir::TypeRange{}, runtime::kExit,
                      mlir::ValueRange{intConstant(location, i32, 1)});
  mlir::ub::UnreachableOp::create(builder_, location);
  return mlir::success();
}

// "true" or "false", as `value` is: what a bool prints as.
mlir::Value MIRLowering::boolText(mlir::Location location, mlir::Value value) {
  auto text = viewOf(builder_.getI8Type());
  auto *region = builder_.getInsertionBlock()->getParent();
  auto *after = &region->emplaceBlock();
  after->addArgument(text, location);
  for (auto word : {"true", "false"}) {
    auto *block = &region->emplaceBlock();
    mlir::OpBuilder::InsertionGuard guard(builder_);
    builder_.setInsertionPointToStart(block);
    auto string = smn::StringOp::create(builder_, location, text, word);
    mlir::cf::BranchOp::create(builder_, location, after,
                               mlir::ValueRange{string.getResult()});
  }
  auto *yes = after->getNextNode();
  mlir::cf::CondBranchOp::create(builder_, location, value, yes,
                                 yes->getNextNode());
  builder_.setInsertionPointToStart(after);
  return after->getArgument(0);
}

// What printf is given for `value`, of a `type` the compiler prints: a
// bool's text, any other as it is.
mlir::Value MIRLowering::printable(mlir::Location location, const Type &type,
                                   mlir::Value value) {
  return type.type_kind == TypeKind::Bool ? boolText(location, value) : value;
}

// Format<T>(value, f), the compiler's, called by name. The Formatter's
// digits are known only when it runs, so a float's are handed to printf
// beside it (`%.*f`), which takes fewer than none as none said.
void MIRLowering::writeBuiltin(mlir::Location location, mlir::Value formatter,
                               const Type &type, mlir::Value value) {
  WriteTo to{false, formatter};
  auto directive = builtin_format_directive(type);
  if (!directive)
    sammine_util::abort("ICE: the compiler has no Format instance for " +
                        type.to_string());
  if (!type.is_float())
    return write(location, to, *directive, printable(location, type, value));
  auto i64 = builder_.getI64Type();
  auto digits = mlir::arith::SubIOp::create(
      builder_, location,
      mlir::arith::ShRSIOp::create(
          builder_, location, formatter,
          intConstant(location, i64, kPrecisionShift)),
      intConstant(location, i64, 1));
  auto narrow = mlir::arith::TruncIOp::create(builder_, location,
                                              builder_.getI32Type(), digits);
  write(location, to, "%.*f", {narrow, value});
}

// print, eprint, write and panic. The format becomes C's: text as it is, a
// `%` in it one that prints a `%`, and each `{}` whose argument the
// compiler's instance of Format prints the directive that does. An argument
// a program's instance prints splits it: what came before is written, then
// the instance's method is called with the argument and a Formatter, which
// says where this writes and the placeholder's digits. panic writes
// `file:line:col: panic: ` first and a newline last, to stderr, and ends
// the program: the block ends there, unreachable, so no drop is put after
// it and no path through it joins the rest. None has a value.
mlir::FailureOr<mlir::Value> MIRLowering::lowerFormat(FormatExprAST *ast) {
  auto location = loc(ast);
  auto *fp = props_.format(ast->id());
  if (!fp || !fp->format)
    return mlir::failure();
  using Kind = FormatExprAST::Kind;
  bool panics = ast->kind == Kind::Panic;
  WriteTo to{panics || ast->kind == Kind::EPrint, {}};
  if (ast->kind == Kind::Write) {
    auto formatter = lowerValue(ast->arguments.front().get());
    if (mlir::failed(formatter))
      return mlir::failure();
    to.formatter = *formatter;
  }
  // Text is printed as it is: a `%` in it isn't a directive.
  auto verbatim = [](llvm::StringRef text) {
    std::string result;
    for (char c : text)
      result += c == '%' ? std::string("%%") : std::string(1, c);
    return result;
  };
  std::string format;
  llvm::SmallVector<mlir::Value> args;
  auto flush = [&] {
    if (!format.empty())
      write(location, to, format, args);
    format.clear();
    args.clear();
  };
  if (auto range = mlir::dyn_cast<mlir::FileLineColRange>(location);
      range && panics)
    format = verbatim(range.getFilename().getValue()) + ":" +
             std::to_string(range.getStartLine()) + ":" +
             std::to_string(range.getStartColumn() + 1) + ": ";
  if (panics)
    format += "panic: ";
  size_t next = 0;
  for (auto &piece : fp->format->pieces) {
    if (!piece.placeholder) {
      format += verbatim(piece.text);
      continue;
    }
    if (next >= fp->methods.size())
      return mlir::failure();
    auto *arg = ast->arguments[ast->format_index() + 1 + next].get();
    auto &method = fp->methods[next++];
    auto value = lowerValue(arg);
    if (mlir::failed(value))
      return mlir::failure();
    auto type = arg->get_type();
    if (method.is<Reference::Builtin>()) {
      auto directive = builtin_format_directive(type);
      if (!directive)
        return mlir::failure();
      format += piece.precision
                    ? "%." + std::to_string(*piece.precision) + "f"
                    : std::string(*directive);
      args.push_back(printable(loc(arg), type, *value));
      continue;
    }
    auto *instance = method.as<Reference::Function>();
    if (!instance)
      return mlir::failure();
    flush();
    smn::CallOp::create(
        builder_, loc(arg), mlir::TypeRange{}, props_.symbol(instance->name),
        mlir::ValueRange{*value, formatterFor(loc(arg), to, piece.precision)},
        "{}");
  }
  if (panics) {
    if (mlir::failed(fail(location, format + "\n", args)))
      return mlir::failure();
    return mlir::Value();
  }
  flush();
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
