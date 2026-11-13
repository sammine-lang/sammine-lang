#include "dialect/ViewCheck.h"
#include "dialect/LambdaSets.h"
#include "dialect/OwnershipPaths.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/SymbolTable.h"
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

/// Adds to `set` each of `items` it hasn't; true if it grew.
bool addAll(llvm::SmallVectorImpl<unsigned> &set,
            llvm::ArrayRef<unsigned> items) {
  bool grew = false;
  for (auto item : items)
    if (!llvm::is_contained(set, item)) {
      set.push_back(item);
      grew = true;
    }
  return grew;
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

/// Where closures that must stay in the call that made them go: one that
/// bound a view, or one that keeps what it binds in its maker's frame
/// (smn::ClosureOp::bindsInline). Each is followed from where it is made,
/// through what is built of it, block arguments, slots, pointers to them
/// and calls, to where it would leave the call: a return, a store through a
/// pointer, or a call that lets it go. Across functions it goes by summaries:
/// which parameters a function lets escape and which it returns, to a fixed
/// point over the module. A call through a function value lets its arguments
/// escape.
class ClosureEscapes {
public:
  explicit ClosureEscapes(mlir::ModuleOp module)
      : module_(module), symbols_(module) {}

  /// Reports each escape; false if there was one.
  bool check() {
    llvm::SmallVector<mlir::func::FuncOp> funcs;
    for (auto func : module_.getOps<mlir::func::FuncOp>())
      if (smn::definesMir(func))
        funcs.push_back(func);
    for (bool changed = true; changed;) {
      changed = false;
      for (auto func : funcs)
        changed |= analyze(func, /*report=*/false);
    }
    for (auto func : funcs)
      analyze(func, /*report=*/true);
    return !failed_;
  }

private:
  /// What a value may hold that must not leave the call: a closure made in
  /// this function, or what the function's parameters hold.
  struct Taint {
    smn::ClosureOp closure;
    llvm::SmallVector<unsigned, 2> params;

    bool empty() const { return !closure && params.empty(); }
    bool join(const Taint &other) {
      bool changed = false;
      if (!closure && other.closure) {
        closure = other.closure;
        changed = true;
      }
      return addAll(params, other.params) || changed;
    }
  };
  /// What a function does with what its parameters hold.
  struct Summary {
    llvm::SmallVector<unsigned, 2> escapes, returns;
  };

  mlir::ModuleOp module_;
  mlir::SymbolTable symbols_;
  llvm::DenseMap<mlir::Operation *, Summary> summaries_;
  bool failed_ = false;

  static bool bindsView(smn::ClosureOp closure) {
    return llvm::any_of(closure.getBound(),
                        [](mlir::Value v) { return isView(v.getType()); });
  }

  /// The summary of the function `call` calls, or null for one MIR doesn't
  /// define: C, or another module's, which may let anything escape.
  const Summary *summaryOf(smn::CallOp call) {
    auto callee = symbols_.lookup<mlir::func::FuncOp>(call.getCallee());
    if (!smn::definesMir(callee))
      return nullptr;
    return &summaries_[callee];
  }

  static std::string what(smn::ClosureOp closure) {
    return "a partial application of '" + writtenName(closure) + "' that " +
           (bindsView(closure) ? "captured a view (ptr<T>)"
                               : "can hold itself through what it binds, so "
                                 "keeps that in this call's frame");
  }

  void error(mlir::Location at, const std::string &msg) {
    failed_ = true;
    mlir::emitError(at, msg);
  }

  /// Taints the function's values, then finds where they leave it. With
  /// `report`, each closure made here that leaves is an error; either way,
  /// the parameters whose closures leave go in its summary. True if the
  /// summary grew.
  bool analyze(mlir::func::FuncOp func, bool report) {
    llvm::DenseMap<mlir::Value, Taint> taints;
    auto taintOf = [&](mlir::Value value) -> Taint {
      auto it = taints.find(value);
      return it == taints.end() ? Taint() : it->second;
    };
    auto add = [&](mlir::Value value, const Taint &taint) {
      return !taint.empty() && taints[value].join(taint);
    };
    auto anyOf = [&](mlir::ValueRange values) {
      Taint all;
      for (auto value : values)
        all.join(taintOf(value));
      return all;
    };

    for (auto [i, arg] : llvm::enumerate(func.getArguments()))
      if (holdsFunctionValue(arg.getType()))
        add(arg, Taint{{}, {static_cast<unsigned>(i)}});

    for (bool changed = true; changed;) {
      changed = false;
      func.walk([&](mlir::Operation *op) {
        if (auto closure = mlir::dyn_cast<smn::ClosureOp>(op)) {
          auto taint = anyOf(op->getOperands());
          // A closure of a lambda set that becomes what it binds carries it
          // by value whatever its type's words (LambdaSets).
          if (bindsView(closure) ||
              (!closure.bindsInline() && !closure->hasAttr(smn::kByValueAttr)))
            taint.closure = closure;
          changed |= add(closure.getResult(), taint);
        } else if (mlir::isa<smn::AggregateOp, smn::VariantOp, smn::LetOp,
                             smn::ExtractOp, smn::PayloadOp, smn::ElementOp>(
                       op)) {
          for (auto result : op->getResults())
            changed |= add(result, anyOf(op->getOperands()));
        } else if (auto store = mlir::dyn_cast<smn::StoreOp>(op)) {
          changed |=
              add(placeRoot(store.getPlace()), taintOf(store.getValue()));
        } else if (mlir::isa<smn::CopyOp, smn::MoveOp, smn::AddrOp, smn::LendOp,
                             smn::DerefOp, smn::ThroughOp>(op)) {
          // What is read from a place holds what the place does, and so
          // does a pointer to it, and the place a pointer is reached to.
          changed |=
              add(op->getResult(0), taintOf(placeRoot(op->getOperand(0))));
        } else if (auto call = mlir::dyn_cast<smn::CallOp>(op)) {
          // What the callee returns of what it was given comes back.
          if (auto *summary = summaryOf(call)) {
            Taint back;
            for (auto i : summary->returns)
              if (i < call.getArgs().size())
                back.join(taintOf(call.getArgs()[i]));
            for (auto result : call.getResults())
              changed |= add(result, back);
          }
        } else if (auto branch = mlir::dyn_cast<mlir::BranchOpInterface>(op)) {
          smn::forEachBranchEdge(
              branch, [&](mlir::Value operand, mlir::BlockArgument arg) {
                changed |= add(arg, taintOf(operand));
              });
        }
      });
    }

    Summary summary = summaries_[func];
    // What `taint` holds leaves the call at `at`: its parameters go in
    // `into`, a closure is reported.
    auto leaves = [&](const Taint &taint, mlir::Location at,
                      llvm::StringRef how, llvm::StringRef when,
                      llvm::SmallVectorImpl<unsigned> &into) {
      addAll(into, taint.params);
      if (report && taint.closure)
        error(at, how.str() + " " + what(taint.closure) + " (may dangle " +
                      when.str() + ")");
    };

    func.walk([&](mlir::Operation *op) {
      if (auto ret = mlir::dyn_cast<mlir::func::ReturnOp>(op)) {
        leaves(anyOf(ret.getOperands()), ret.getLoc(), "Cannot return",
               "after return", summary.returns);
      } else if (auto store = mlir::dyn_cast<smn::StoreOp>(op)) {
        if (reachedThrough(store.getPlace()))
          leaves(taintOf(store.getValue()), store.getLoc(),
                 "Cannot store, through a pointer,",
                 "once the function returns", summary.escapes);
      } else if (auto call = mlir::dyn_cast<smn::CallOp>(op)) {
        auto *callee = summaryOf(call);
        for (auto [i, arg] : llvm::enumerate(call.getArgs())) {
          if (callee &&
              !llvm::is_contained(callee->escapes, static_cast<unsigned>(i)))
            continue;
          leaves(taintOf(arg), call.getLoc(),
                 "Cannot pass to '" + call.getCallee().str() + "'",
                 "once this function returns: '" + call.getCallee().str() +
                     "' keeps it",
                 summary.escapes);
        }
      } else if (auto indirect = mlir::dyn_cast<smn::CallIndirectOp>(op)) {
        for (auto arg : indirect.getArgs())
          leaves(taintOf(arg), indirect.getLoc(),
                 "Cannot pass to a function value",
                 "once this function returns: the function called may keep "
                 "it",
                 summary.escapes);
      }
    });

    auto &old = summaries_[func];
    bool grew = summary.escapes.size() != old.escapes.size() ||
                summary.returns.size() != old.returns.size();
    old = summary;
    return grew;
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
    if (smn::definesMir(func))
      check.check(func);
  bool stays = ClosureEscapes(module).check();
  return mlir::failure(check.failed() || !stays);
}

std::unique_ptr<mlir::Pass> createCheckViewsPass() {
  return std::make_unique<CheckViewsPass>();
}

void registerCheckViewsPass() { mlir::PassRegistration<CheckViewsPass>(); }

} // namespace sammine_lang
