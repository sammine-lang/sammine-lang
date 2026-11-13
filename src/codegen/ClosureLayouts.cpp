#include "codegen/ClosureLayouts.h"
#include "codegen/MIRTypes.h"
#include "dialect/SammineDialect.h"

namespace sammine_lang {

ClosureLayouts::ClosureLayouts(mlir::MLIRContext *ctx)
    : shapes_(std::make_unique<MIRTypes>(ctx)) {}

ClosureLayouts::~ClosureLayouts() = default;

mlir::Type ClosureLayouts::shapeOf(const Type &fn) const {
  auto shape = shapes_->mirType(fn);
  return shape ? *shape : mlir::Type();
}

void ClosureLayouts::addClosure(const Type &closure, std::vector<Type> bound) {
  if (auto shape = shapeOf(closure))
    closures_[shape].push_back(std::move(bound));
}

std::optional<int64_t> ClosureLayouts::words(const Type &fn,
                                             const MIRTypes &types) const {
  auto shape = shapeOf(fn);
  if (!shape)
    return 0;
  if (auto known = words_.find(shape); known != words_.end())
    return known->second;
  auto closures = closures_.find(shape);
  if (closures == closures_.end())
    return 0;
  // Sizing this type needs this type's size: a closure of it binds one, or
  // binds one through other closures. The closure that meets the cycle
  // keeps what it binds in its maker's frame instead (smn::bindsInline).
  if (!sizing_.insert(shape).second)
    return std::nullopt;

  int64_t most = 0;
  for (auto &bound : closures->second) {
    // In its maker's frame, the closure carries a pointer to what it binds.
    auto mir = types.mirTypes(bound);
    most = std::max(
        most,
        mir ? smn::wordsFor(smn::TupleType::get(shape.getContext(), *mir)) : 1);
  }
  sizing_.erase(shape);
  words_[shape] = most;
  return most;
}

} // namespace sammine_lang
