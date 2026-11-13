#include "dialect/OwnershipPaths.h"
#include "dialect/SammineDialect.h"

#include "llvm/ADT/TypeSwitch.h"

//! \file OwnershipPaths.cpp
//! \brief Which part of what an op's value or place names, and its name in
//! the source.

namespace sammine_lang::ownership {

mlir::Type typeOf(const Path &path) {
  auto type = path.root.getType();
  if (auto place = mlir::dyn_cast<smn::PlaceType>(type))
    type = place.getValueType();
  for (auto &step : path.steps) {
    switch (step.kind) {
    case Step::Field:
      if (auto st = mlir::dyn_cast<smn::StructType>(type))
        type = st.getFields()[static_cast<size_t>(step.a)];
      else
        type = mlir::cast<smn::TupleType>(type)
                   .getElements()[static_cast<size_t>(step.a)];
      break;
    case Step::Payload:
      type = mlir::cast<smn::EnumType>(type).getPayload(
          static_cast<uint64_t>(step.a))[static_cast<size_t>(step.b)];
      break;
    case Step::Pointee:
      type = mlir::cast<smn::PtrType>(type).getPointee();
      break;
    case Step::Element:
      type = mlir::cast<smn::ArrayType>(type).getElement();
      break;
    }
  }
  return type;
}

bool isOwnedPtr(mlir::Type type) {
  auto ptr = mlir::dyn_cast<smn::PtrType>(type);
  return ptr && ptr.isOwned();
}

llvm::SmallVector<Step> partsOf(mlir::Type type) {
  llvm::SmallVector<Step> parts;
  auto fields = [&](llvm::ArrayRef<mlir::Type> types) {
    for (auto [i, t] : llvm::enumerate(types))
      if (smn::ownsMemory(t))
        parts.push_back({Step::Field, static_cast<int64_t>(i)});
  };
  if (auto st = mlir::dyn_cast<smn::StructType>(type)) {
    fields(st.getFields());
  } else if (auto tuple = mlir::dyn_cast<smn::TupleType>(type)) {
    fields(tuple.getElements());
  } else if (auto en = mlir::dyn_cast<smn::EnumType>(type)) {
    for (uint64_t v = 0; v < en.getVariants().size(); ++v)
      for (auto [i, t] : llvm::enumerate(en.getPayload(v)))
        if (smn::ownsMemory(t))
          parts.push_back({Step::Payload, static_cast<int64_t>(v),
                           static_cast<int64_t>(i)});
  } else if (auto array = mlir::dyn_cast<smn::ArrayType>(type)) {
    if (smn::ownsMemory(array.getElement()))
      parts.push_back({Step::Element});
  } else if (auto ptr = mlir::dyn_cast<smn::PtrType>(type)) {
    if (ptr.isBox() && smn::ownsMemory(ptr.getPointee()))
      parts.push_back({Step::Pointee});
  }
  return parts;
}

namespace {

/// What an op naming a part of another value or place names it from, and
/// the step to the part. Nullopt for a pointee through a pointer that
/// doesn't own.
struct PartOf {
  mlir::Value whole;
  Step step;
};

std::optional<PartOf> partOf(mlir::Operation *op) {
  auto pointee = [](mlir::Value pointer,
                    mlir::Type type) -> std::optional<PartOf> {
    if (!isOwnedPtr(type))
      return std::nullopt;
    return PartOf{pointer, {Step::Pointee}};
  };
  return llvm::TypeSwitch<mlir::Operation *, std::optional<PartOf>>(op)
      .Case([](smn::ExtractOp op) {
        return PartOf{op.getAggregate(),
                      {Step::Field, static_cast<int64_t>(op.getIndex())}};
      })
      .Case([](smn::PayloadOp op) {
        return PartOf{op.getValue(),
                      {Step::Payload, static_cast<int64_t>(op.getVariant()),
                       static_cast<int64_t>(op.getField())}};
      })
      .Case([](smn::ElementOp op) {
        return PartOf{op.getArray(), {Step::Element}};
      })
      .Case([](smn::FieldOp op) {
        return PartOf{op.getBase(),
                      {Step::Field, static_cast<int64_t>(op.getIndex())}};
      })
      .Case(
          [](smn::ElemOp op) { return PartOf{op.getBase(), {Step::Element}}; })
      .Case([&](smn::ThroughOp op) {
        return pointee(op.getPlace(), op.getPlace().getType().getValueType());
      })
      .Case([&](smn::DerefOp op) {
        return pointee(op.getPtr(), op.getPtr().getType());
      })
      .Default([](mlir::Operation *) { return std::nullopt; });
}

} // namespace

std::optional<Path> pathOf(mlir::Value value) {
  auto *op = value.getDefiningOp();
  if (op && mlir::isa<smn::LocalOp>(op))
    return Path{value, {}};
  // p[i] is past what the analysis follows.
  if (op && mlir::isa<smn::PtrElemOp>(op))
    return std::nullopt;
  if (!op ||
      !mlir::isa<smn::ExtractOp, smn::PayloadOp, smn::ElementOp, smn::FieldOp,
                 smn::ElemOp, smn::DerefOp, smn::ThroughOp>(op)) {
    if (!smn::ownsMemory(value.getType()))
      return std::nullopt;
    return Path{value, {}};
  }
  auto part = partOf(op);
  if (!part)
    return std::nullopt;
  auto base = pathOf(part->whole);
  if (!base)
    return std::nullopt;
  return base->child(part->step);
}

mlir::Location definedAt(mlir::Value value) {
  if (auto *op = value.getDefiningOp())
    return op->getLoc();
  return mlir::cast<mlir::BlockArgument>(value).getLoc();
}

std::optional<std::string> bindingName(mlir::Value root) {
  if (auto let = root.getDefiningOp<smn::LetOp>())
    return let.getName().str();
  if (auto local = root.getDefiningOp<smn::LocalOp>())
    return local.getName().str();
  if (auto move = root.getDefiningOp<smn::MoveOp>())
    if (auto place = pathOf(move.getPlace()); place && bindingName(place->root))
      return nameOf(*place);
  return std::nullopt;
}

std::string nameOf(const Path &path) {
  auto root = bindingName(path.root);
  if (!root)
    return "";
  std::string name = *root;
  Path prefix{path.root, {}};
  for (auto &step : path.steps) {
    auto type = typeOf(prefix);
    switch (step.kind) {
    case Step::Field:
      if (auto st = mlir::dyn_cast<smn::StructType>(type))
        name +=
            "." +
            st.getFieldNames()[static_cast<size_t>(step.a)].getValue().str();
      else
        name += "." + std::to_string(step.a);
      break;
    case Step::Payload:
      name += "." +
              mlir::cast<smn::EnumType>(type)
                  .getVariantNames()[static_cast<size_t>(step.a)]
                  .getValue()
                  .str() +
              "." + std::to_string(step.b);
      break;
    case Step::Pointee:
      name = "*" + name;
      break;
    case Step::Element:
      name += "[*]";
      break;
    }
    prefix = prefix.child(step);
  }
  return name;
}

} // namespace sammine_lang::ownership
