#include "dialect/ViewCheck.h"
#include "dialect/OwnershipPaths.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"

//! \file ViewCheck.cpp
//! \brief Views checked op by op: a store or an `&` is judged by the pointer
//! the place it names is reached through, the nearest one.

namespace sammine_lang {

namespace {

/// The op that reaches through a pointer to get to `place`, the nearest
/// one: `*p`, or `*e` of a pointer held in a place. Null if the place is a
/// slot of its own, reached through no pointer.
mlir::Operation *reachedThrough(mlir::Value place) {
  while (auto *op = place.getDefiningOp()) {
    if (mlir::isa<smn::DerefOp, smn::ThroughOp>(op))
      return op;
    if (auto field = mlir::dyn_cast<smn::FieldOp>(op))
      place = field.getBase();
    else if (auto elem = mlir::dyn_cast<smn::ElemOp>(op))
      place = elem.getBase();
    else if (auto ptrElem = mlir::dyn_cast<smn::PtrElemOp>(op))
      place = ptrElem.getBase();
    else
      return nullptr;
  }
  return nullptr;
}

/// The pointer `through`, a deref or a through, goes through.
smn::PtrType pointerOf(mlir::Operation *through) {
  if (auto deref = mlir::dyn_cast<smn::DerefOp>(through))
    return deref.getPtr().getType();
  return mlir::cast<smn::PtrType>(
      mlir::cast<smn::ThroughOp>(through).getPlace().getType().getValueType());
}

/// The binding that names the pointer `through` goes through, if MIR knows
/// one: it names a binding of a view only if the view is in a slot.
std::optional<std::string> pointerName(mlir::Operation *through) {
  return ownership::bindingName(through->getOperand(0));
}

class ViewCheck {
public:
  bool failed() const { return failed_; }

  void check(mlir::Operation *op) {
    if (auto store = mlir::dyn_cast<smn::StoreOp>(op))
      checkWrite(store.getPlace());
    else if (auto addr = mlir::dyn_cast<smn::AddrOp>(op))
      checkAddr(addr);
  }

private:
  bool failed_ = false;

  void error(mlir::Location at, const std::string &msg) {
    failed_ = true;
    mlir::emitError(at, msg);
  }

  /// Only an owner writes what it points at.
  void checkWrite(mlir::Value place) {
    auto *through = reachedThrough(place);
    if (!through || pointerOf(through).isOwned())
      return;
    auto name = pointerName(through);
    error(through->getLoc(),
          "Cannot write through non-owning pointer" +
              (name ? " '" + *name + "'" : std::string()) +
              " — only an owned pointer can be written");
  }

  /// `&` of an owned pointer would alias its one owner; of what one points
  /// at, it could outlive the owner's drop.
  void checkAddr(smn::AddrOp addr) {
    auto place = addr.getPlace();
    if (ownership::isOwnedPtr(place.getType().getValueType())) {
      error(addr.getLoc(), "Cannot take address of a linear value — linear "
                           "values cannot be aliased");
      return;
    }
    auto *through = reachedThrough(place);
    if (through && pointerOf(through).isOwned())
      error(through->getLoc(), "Cannot take the address of memory owned by a "
                               "linear pointer — it could dangle after drop");
  }
};

struct CheckViewsPass
    : public mlir::PassWrapper<CheckViewsPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckViewsPass)

  llvm::StringRef getArgument() const override { return "sammine-check-views"; }
  llvm::StringRef getDescription() const override {
    return "Check that nothing is written through a view, and that & makes "
           "no view of owned memory";
  }
  void runOnOperation() override {
    if (mlir::failed(checkViews(getOperation())))
      signalPassFailure();
  }
};

} // namespace

mlir::LogicalResult checkViews(mlir::ModuleOp module) {
  ViewCheck check;
  for (auto func : module.getOps<mlir::func::FuncOp>())
    if (!func.isDeclaration() && func->hasAttr(smn::kMirAttr))
      func.walk([&](mlir::Operation *op) { check.check(op); });
  return mlir::failure(check.failed());
}

std::unique_ptr<mlir::Pass> createCheckViewsPass() {
  return std::make_unique<CheckViewsPass>();
}

void registerCheckViewsPass() { mlir::PassRegistration<CheckViewsPass>(); }

} // namespace sammine_lang
