#include "codegen/MIRLowering.h"
#include "codegen/RangeIteration.h"
#include "util/Utilities.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"

//! \file MIRLoweringControlFlow.cpp
//! \brief `if`, `case`, `for`, `&&` and `||`: the blocks they branch between
//! and the value each arm leaves.

namespace sammine_lang {

using namespace AST;

// The type an if's or a case's value has in MIR: none if it is () or never
// reached; failure if MIR has no type for it.
mlir::FailureOr<std::optional<mlir::Type>>
MIRLowering::branchResultType(const Type &type) const {
  if (type.type_kind == TypeKind::Unit || type.type_kind == TypeKind::Never)
    return std::optional<mlir::Type>();
  auto mir = types_.mirType(type);
  if (!mir)
    return mlir::failure();
  return mir;
}

MIRLowering::Arm MIRLowering::armOf(mlir::Block *block, BlockAST *body,
                                    std::function<void()> bind) {
  return Arm{block, [this, body, bind = std::move(bind)] {
               if (bind)
                 bind();
               return lowerStatements(body);
             }};
}

// Each arm in its block, in a scope of its own. The arms that don't return
// go on to a block after, which takes their value, of `resultType`. There
// is none if every arm returns.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerArms(mlir::Location location,
                       std::optional<mlir::Type> resultType,
                       llvm::ArrayRef<Arm> arms) {
  auto *region = arms.front().block->getParent();
  auto *after = &region->emplaceBlock();
  if (resultType)
    after->addArgument(*resultType, location);
  bool reached = false;
  for (auto &arm : arms) {
    builder_.setInsertionPointToStart(arm.block);
    mlir::FailureOr<mlir::Value> value;
    {
      NameScope scope(*this);
      value = arm.value();
    }
    if (mlir::failed(value))
      return mlir::failure();
    if (terminated())
      continue;
    if (resultType && !*value)
      return mlir::failure();
    mlir::cf::BranchOp::create(builder_, location, after,
                               resultType ? mlir::ValueRange{*value}
                                          : mlir::ValueRange{});
    reached = true;
  }
  if (!reached) {
    after->erase();
    return mlir::Value();
  }
  // After the blocks the arms made.
  continueIn(after);
  return resultType ? after->getArgument(0) : mlir::Value();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerBranch(
    mlir::Location location, mlir::Value cond,
    std::optional<mlir::Type> resultType,
    std::function<mlir::FailureOr<mlir::Value>()> then,
    std::function<mlir::FailureOr<mlir::Value>()> otherwise) {
  auto *region = builder_.getInsertionBlock()->getParent();
  auto *thenBlock = &region->emplaceBlock();
  auto *elseBlock = &region->emplaceBlock();
  mlir::cf::CondBranchOp::create(builder_, location, cond, thenBlock,
                                 elseBlock);
  return lowerArms(location, resultType,
                   {Arm{thenBlock, std::move(then)},
                    Arm{elseBlock, std::move(otherwise)}});
}

// `if c { a } else { b }`: c branches to a block for each side.
mlir::FailureOr<mlir::Value> MIRLowering::lowerIf(IfExprAST *ifExpr) {
  auto resultType = branchResultType(ifExpr->get_type());
  if (mlir::failed(resultType))
    return mlir::failure();
  auto cond = lowerValue(ifExpr->bool_expr.get());
  if (mlir::failed(cond))
    return mlir::failure();
  auto block = [this](BlockAST *body) {
    return [this, body] { return lowerStatements(body); };
  };
  return lowerBranch(loc(ifExpr), *cond, *resultType,
                     block(ifExpr->thenBlockAST.get()),
                     block(ifExpr->elseBlockAST.get()));
}

// `case e { Some(x) => a, None => b }` on an enum: the tag picks the arm's
// block, where the pattern's names are bound to the payload. With no
// wildcard, any other tag goes to the last arm: the arms cover every
// variant.
mlir::FailureOr<mlir::Value> MIRLowering::lowerCase(CaseExprAST *caseExpr) {
  auto scrutineeType = caseExpr->scrutinee->get_type();
  if (caseExpr->arms.empty())
    return mlir::failure();
  if (scrutineeType.type_kind != TypeKind::Enum ||
      std::get<EnumType>(scrutineeType.type_data).is_integer_backed())
    return lowerScalarCase(caseExpr);
  auto et = std::get<EnumType>(scrutineeType.type_data);
  auto resultType = branchResultType(caseExpr->get_type());
  if (mlir::failed(resultType))
    return mlir::failure();
  auto value = lowerValue(caseExpr->scrutinee.get());
  if (mlir::failed(value))
    return mlir::failure();
  auto enumType = mlir::dyn_cast<smn::EnumType>(value->getType());
  if (!enumType)
    return mlir::failure();

  auto location = loc(caseExpr);
  auto tag =
      smn::TagOp::create(builder_, location, builder_.getI32Type(), *value);
  auto *region = builder_.getInsertionBlock()->getParent();
  llvm::SmallVector<Arm> arms;
  llvm::SmallVector<int32_t> tags;
  llvm::SmallVector<mlir::Block *> dests;
  mlir::Block *otherwise = nullptr;
  for (auto &arm : caseExpr->arms) {
    auto &pattern = arm.pattern;
    if (pattern.is_literal)
      return mlir::failure();
    auto *block = &region->emplaceBlock();
    if (pattern.is_wildcard) {
      otherwise = block;
      arms.push_back(armOf(block, arm.body.get()));
      continue;
    }
    tags.push_back(static_cast<int32_t>(pattern.variant_index));
    dests.push_back(block);
    auto &payloadTypes = et.get_variant(pattern.variant_index).payload_types;
    auto mirPayload = enumType.getPayload(pattern.variant_index);
    if (pattern.bindings.size() > payloadTypes.size())
      return mlir::failure();
    // Run by lowerArms, after this loop: it holds only what outlives it.
    arms.push_back(
        armOf(block, arm.body.get(),
            [this, &pattern, mirPayload, scrutinee = *value,
             location] {
              auto offsets = smn::payloadOffsets(mirPayload);
              for (auto [i, name] : llvm::enumerate(pattern.bindings)) {
                bind(name,
                     smn::PayloadOp::create(
                         builder_, location, mirPayload[i], scrutinee,
                         pattern.variant_index, i,
                         static_cast<uint64_t>(offsets[i])),
                     loc(pattern.location));
              }
            }));
  }
  if (!otherwise) {
    otherwise = dests.pop_back_val();
    tags.pop_back();
  } else {
    // A wildcard takes a variant no arm names with its payload, which owns
    // what it holds: bound on the way to the wildcard's body, out of its
    // reach, it is dropped there like any value going out of use.
    llvm::SmallDenseSet<size_t> named;
    for (auto &arm : caseExpr->arms)
      if (!arm.pattern.is_wildcard)
        named.insert(arm.pattern.variant_index);
    for (uint64_t v = 0; v < enumType.getVariants().size(); ++v) {
      auto payload = enumType.getPayload(v);
      if (named.contains(v) || !llvm::any_of(payload, smn::ownsMemory))
        continue;
      auto *block = &region->emplaceBlock();
      mlir::OpBuilder unnamed = mlir::OpBuilder::atBlockEnd(block);
      auto offsets = smn::payloadOffsets(payload);
      for (auto [i, field] : llvm::enumerate(payload))
        if (smn::ownsMemory(field))
          smn::LetOp::create(
              unnamed, location, field, unnamed.getStringAttr("_"),
              smn::PayloadOp::create(unnamed, location, field, *value, v, i,
                                     static_cast<uint64_t>(offsets[i])));
      mlir::cf::BranchOp::create(unnamed, location, otherwise);
      tags.push_back(static_cast<int32_t>(v));
      dests.push_back(block);
    }
  }
  branchOn(location, tag, tags, dests, otherwise, mlir::ValueRange{});
  return lowerArms(location, *resultType, arms);
}

// `for i in a..b { body }`: Range { a, b } goes
// round a loop, each turn calling Iterator<Range<T>, T>::next on it. Some
// gives the next range and i, for the body, then the loop goes round with
// that range; None leaves the loop.
mlir::FailureOr<mlir::Value> MIRLowering::lowerFor(ForExprAST *forExpr) {
  auto *forProps = props_.for_loop(forExpr->id());
  if (!forProps || !forProps->range_type)
    return mlir::failure();
  auto elementType = forExpr->start->get_type();
  auto iteration = rangeIteration(*forProps->range_type, elementType);
  auto rangeType = types_.mirType(*forProps->range_type);
  auto optionType = types_.mirType(iteration.option);
  auto stepType = types_.mirType(iteration.step);
  auto element = types_.mirType(elementType);
  if (!rangeType || !optionType || !stepType || !element ||
      !module_.lookupSymbol<mlir::FunctionOpInterface>(iteration.next))
    return mlir::failure();
  auto location = loc(forExpr);

  auto start = lowerValue(forExpr->start.get());
  if (mlir::failed(start))
    return mlir::failure();
  auto end = lowerValue(forExpr->end.get());
  if (mlir::failed(end))
    return mlir::failure();
  auto range = smn::AggregateOp::create(builder_, location, *rangeType,
                                        mlir::ValueRange{*start, *end});

  auto *region = builder_.getInsertionBlock()->getParent();
  auto *header = &region->emplaceBlock();
  header->addArgument(*rangeType, location);
  mlir::cf::BranchOp::create(builder_, location, header,
                             mlir::ValueRange{range});

  builder_.setInsertionPointToStart(header);
  auto option =
      smn::CallOp::create(builder_, location, mlir::TypeRange{*optionType},
                          iteration.next, header->getArgument(0), "for")
          .getResult(0);
  auto tag =
      smn::TagOp::create(builder_, location, builder_.getI32Type(), option);
  auto some = intConstant(location, builder_.getI32Type(), 0);
  auto isSome = mlir::arith::CmpIOp::create(
      builder_, location, mlir::arith::CmpIPredicate::eq, tag, some);
  auto *body = &region->emplaceBlock();
  auto *exit = &region->emplaceBlock();
  mlir::cf::CondBranchOp::create(builder_, location, isSome, body, exit);

  builder_.setInsertionPointToStart(body);
  auto step = smn::PayloadOp::create(builder_, location, *stepType, option,
                                     /*variant=*/0, /*field=*/0, /*offset=*/0);
  auto next = smn::ExtractOp::create(builder_, location, *rangeType, step, 0);
  mlir::FailureOr<mlir::Value> value;
  {
    NameScope scope(*this);
    bind(forExpr->loop_var,
         smn::ExtractOp::create(builder_, location, *element, step, 1),
         location);
    value = lowerStatements(forExpr->body.get());
  }
  if (mlir::failed(value))
    return mlir::failure();
  if (!terminated())
    mlir::cf::BranchOp::create(builder_, location, header,
                               mlir::ValueRange{next});

  continueIn(exit);
  return mlir::Value();
}

// `case n { 0 => a, 1 => b, _ => c }` on an integer, char or bool, or on an
// integer-backed enum's variants: a cf.switch on the value, to each arm's
// block. With no wildcard, any other value goes to the last arm: the arms
// cover them all. An arm for a value an earlier arm took is never reached.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerScalarCase(CaseExprAST *caseExpr) {
  auto scrutineeType = caseExpr->scrutinee->get_type();
  std::optional<EnumType> intEnum;
  if (scrutineeType.type_kind == TypeKind::Enum)
    intEnum = std::get<EnumType>(scrutineeType.type_data);
  auto resultType = branchResultType(caseExpr->get_type());
  if (mlir::failed(resultType))
    return mlir::failure();
  auto value = lowerValue(caseExpr->scrutinee.get());
  if (mlir::failed(value))
    return mlir::failure();
  auto flagType = mlir::dyn_cast<mlir::IntegerType>(value->getType());
  if (!flagType)
    return mlir::failure();

  // The value each arm matches, as its pattern says it.
  using LK = CasePattern::LiteralKind;
  auto armValue = [&](const CasePattern &pattern) -> std::optional<int64_t> {
    if (intEnum)
      return intEnum->get_variant(pattern.variant_index).discriminant_value;
    switch (pattern.literal_kind) {
    case LK::Bool:
      return pattern.literal_value == "true" ? 1 : 0;
    case LK::Char:
      return static_cast<uint8_t>(pattern.literal_value[0]);
    case LK::Integer:
      return sammine_util::literal_int_bits(pattern.literal_value);
    default:
      return std::nullopt;
    }
  };

  auto *region = builder_.getInsertionBlock()->getParent();
  llvm::SmallVector<Arm> arms;
  llvm::SmallVector<llvm::APInt> values;
  llvm::SmallVector<mlir::Block *> dests;
  mlir::Block *otherwise = nullptr;
  for (auto &arm : caseExpr->arms) {
    auto *block = &region->emplaceBlock();
    arms.push_back(armOf(block, arm.body.get()));
    if (arm.pattern.is_wildcard) {
      otherwise = block;
      break;
    }
    if (!intEnum && !arm.pattern.is_literal)
      return mlir::failure();
    auto matched = armValue(arm.pattern);
    if (!matched)
      return mlir::failure();
    llvm::APInt bits(flagType.getWidth(), static_cast<uint64_t>(*matched),
                     /*isSigned=*/true, /*implicitTrunc=*/true);
    if (llvm::is_contained(values, bits))
      continue;
    values.push_back(bits);
    dests.push_back(block);
  }
  if (!otherwise) {
    if (dests.empty())
      return mlir::failure();
    otherwise = dests.pop_back_val();
    values.pop_back();
  }
  auto location = loc(caseExpr);
  branchOn(location, *value, values, dests, otherwise, mlir::ValueRange{});
  return lowerArms(location, *resultType, arms);
}

// Branches on `flag`: to the block of the value it equals, else to
// `otherwise`, passed `otherwiseArgs`. With no values that is a plain
// branch: MLIR's translation to LLVM IR can't take a switch with no cases
// whose default takes arguments (upstream/mlir-switch-no-cases-phi).
void MIRLowering::branchOn(mlir::Location location, mlir::Value flag,
                           llvm::ArrayRef<llvm::APInt> values,
                           llvm::ArrayRef<mlir::Block *> dests,
                           mlir::Block *otherwise,
                           mlir::ValueRange otherwiseArgs) {
  if (values.empty()) {
    mlir::cf::BranchOp::create(builder_, location, otherwise, otherwiseArgs);
    return;
  }
  auto caseValues = mlir::DenseIntElementsAttr::get(
      mlir::VectorType::get({static_cast<int64_t>(values.size())},
                            flag.getType()),
      values);
  llvm::SmallVector<mlir::ValueRange> noOperands(dests.size());
  mlir::cf::SwitchOp::create(builder_, location, flag, otherwiseArgs,
                             noOperands, caseValues, otherwise, dests);
}

void MIRLowering::branchOn(mlir::Location location, mlir::Value flag,
                           llvm::ArrayRef<int32_t> tags,
                           llvm::ArrayRef<mlir::Block *> dests,
                           mlir::Block *otherwise,
                           mlir::ValueRange otherwiseArgs) {
  llvm::SmallVector<llvm::APInt> values;
  for (auto tag : tags)
    values.emplace_back(32, static_cast<uint64_t>(tag), /*isSigned=*/true);
  branchOn(location, flag, values, dests, otherwise, otherwiseArgs);
}

// `a && b` is `if a { b } else { false }`, `a || b` is
// `if a { true } else { b }`: b is read only on the side that needs it.
mlir::FailureOr<mlir::Value> MIRLowering::lowerLogical(BinaryExprAST *bin) {
  auto lhs = lowerValue(bin->LHS.get());
  if (mlir::failed(lhs))
    return mlir::failure();
  bool isAnd = bin->Op->tok_type == TokAND;
  auto location = loc(bin);
  std::function<mlir::FailureOr<mlir::Value>()> rhs = [this, bin] {
    return lowerValue(bin->RHS.get());
  };
  std::function<mlir::FailureOr<mlir::Value>()> known = [this, location,
                                                         isAnd] {
    return mlir::FailureOr<mlir::Value>(
        intConstant(location, builder_.getI1Type(), isAnd ? 0 : 1));
  };
  return lowerBranch(location, *lhs, builder_.getI1Type(),
                     isAnd ? rhs : known, isAnd ? known : rhs);
}

} // namespace sammine_lang
