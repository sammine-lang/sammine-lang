#include "dialect/ViewCheck.h"
#include "dialect/OwnershipPaths.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"

#include "llvm/ADT/DenseMap.h"

//! \file ViewCheck.cpp
//! \brief Views checked op by op: a store or an `&` is judged by the pointer
//! the place it names is reached through, the nearest one. And closures: a
//! closure binds no value that owns memory, and one that bound a view
//! doesn't leave the function, as a value or through a pointer.

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

/// The slot or pointee a place is part of: past its fields and elements.
mlir::Value placeRoot(mlir::Value place) {
  while (auto *op = place.getDefiningOp()) {
    if (auto field = mlir::dyn_cast<smn::FieldOp>(op))
      place = field.getBase();
    else if (auto elem = mlir::dyn_cast<smn::ElemOp>(op))
      place = elem.getBase();
    else if (auto ptrElem = mlir::dyn_cast<smn::PtrElemOp>(op))
      place = ptrElem.getBase();
    else
      break;
  }
  return place;
}

/// A function's name as the source writes it: its symbol, without the type
/// arguments a generic copy's carries.
std::string writtenName(smn::ClosureOp closure) {
  auto name = closure.getCallee().str();
  return name.substr(0, name.find('<'));
}

bool isView(mlir::Type type) {
  auto ptr = mlir::dyn_cast<smn::PtrType>(type);
  return ptr && !ptr.isOwned();
}

class ViewCheck {
public:
  bool failed() const { return failed_; }

  void check(mlir::func::FuncOp func) {
    func.walk([&](mlir::Operation *op) {
      if (auto store = mlir::dyn_cast<smn::StoreOp>(op))
        checkWrite(store.getPlace());
      else if (auto addr = mlir::dyn_cast<smn::AddrOp>(op))
        checkAddr(addr);
      else if (auto closure = mlir::dyn_cast<smn::ClosureOp>(op))
        checkBound(closure);
    });
    checkEscapes(func);
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

  void checkBound(smn::ClosureOp closure);
  void checkEscapes(mlir::func::FuncOp func);

  /// `&` of an owned pointer would alias its one owner; of what one points
  /// at, it could outlive the owner's drop.
  void checkAddr(smn::AddrOp addr) {
    auto place = addr.getPlace();
    if (ownership::isOwnedPtr(place.getType().getValueType())) {
      error(addr.getLoc(), "Cannot take the address of an owned pointer: it "
                           "would alias its one owner");
      return;
    }
    auto *through = reachedThrough(place);
    if (through && pointerOf(through).isOwned())
      error(through->getLoc(), "Cannot take the address of memory an owned "
                               "pointer holds: it could dangle once that is "
                               "dropped");
  }
};

// --- Closures -------------------------------------------------------------

/// A closure can be called more than once, and each call would hand what
/// it bound on again.
void ViewCheck::checkBound(smn::ClosureOp closure) {
  for (auto bound : closure.getBound()) {
    if (!smn::ownsMemory(bound.getType()))
      continue;
    auto path = ownership::pathOf(bound);
    auto name = path ? ownership::nameOf(*path) : "";
    error(closure.getLoc(),
          "Cannot bind " + (name.empty() ? "a value" : "'" + name + "'") +
              " that owns memory into a partial application of '" +
              writtenName(closure) +
              "': the closure can be called more than once");
  }
}

/// Every value and slot holding a closure that bound a view, followed from
/// each such closure through what is built of it, block arguments and
/// slots, to where it would leave the function: a return, or a store
/// through a pointer. Each is reported with the closure it came from.
void ViewCheck::checkEscapes(mlir::func::FuncOp func) {
  llvm::DenseMap<mlir::Value, smn::ClosureOp> from;
  auto taint = [&](mlir::Value value, smn::ClosureOp closure, bool &changed) {
    if (closure && from.try_emplace(value, closure).second)
      changed = true;
  };
  auto taintedBy = [&](mlir::Value value) -> smn::ClosureOp {
    auto it = from.find(value);
    return it == from.end() ? smn::ClosureOp() : it->second;
  };
  auto anyOf = [&](mlir::ValueRange values) -> smn::ClosureOp {
    for (auto value : values)
      if (auto closure = taintedBy(value))
        return closure;
    return {};
  };
  for (bool changed = true; changed;) {
    changed = false;
    func.walk([&](mlir::Operation *op) {
      if (auto closure = mlir::dyn_cast<smn::ClosureOp>(op)) {
        bool bindsView = llvm::any_of(closure.getBound(), [](mlir::Value v) {
          return isView(v.getType());
        });
        taint(closure.getResult(),
              bindsView ? closure : anyOf(op->getOperands()), changed);
      } else if (mlir::isa<smn::AggregateOp, smn::VariantOp, smn::LetOp,
                           smn::ExtractOp, smn::PayloadOp, smn::ElementOp>(
                     op)) {
        for (auto result : op->getResults())
          taint(result, anyOf(op->getOperands()), changed);
      } else if (auto store = mlir::dyn_cast<smn::StoreOp>(op)) {
        taint(placeRoot(store.getPlace()), taintedBy(store.getValue()),
              changed);
      } else if (mlir::isa<smn::CopyOp, smn::MoveOp>(op)) {
        taint(op->getResult(0), taintedBy(placeRoot(op->getOperand(0))),
              changed);
      } else if (auto branch = mlir::dyn_cast<mlir::BranchOpInterface>(op)) {
        for (unsigned i = 0; i < branch->getNumSuccessors(); ++i)
          for (auto [operand, arg] :
               llvm::zip(branch.getSuccessorOperands(i).getForwardedOperands(),
                         branch->getSuccessor(i)->getArguments()))
            taint(arg, taintedBy(operand), changed);
      }
    });
  }
  auto message = [](llvm::StringRef what, smn::ClosureOp closure,
                    llvm::StringRef when) {
    return what.str() + " a partial application of '" + writtenName(closure) +
           "' that captured a view (ptr<T>) (may dangle " + when.str() + ")";
  };
  func.walk([&](mlir::Operation *op) {
    if (auto ret = mlir::dyn_cast<mlir::func::ReturnOp>(op)) {
      if (auto closure = anyOf(ret.getOperands()))
        error(ret.getLoc(), message("Cannot return", closure, "after return"));
    } else if (auto store = mlir::dyn_cast<smn::StoreOp>(op)) {
      if (auto closure = taintedBy(store.getValue());
          closure && reachedThrough(store.getPlace()))
        error(store.getLoc(), message("Cannot store, through a pointer,",
                                      closure, "once the function returns"));
    }
  });
}

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
      check.check(func);
  return mlir::failure(check.failed());
}

std::unique_ptr<mlir::Pass> createCheckViewsPass() {
  return std::make_unique<CheckViewsPass>();
}

void registerCheckViewsPass() { mlir::PassRegistration<CheckViewsPass>(); }

} // namespace sammine_lang
