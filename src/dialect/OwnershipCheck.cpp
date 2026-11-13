#include "dialect/OwnershipCheck.h"
#include "dialect/OwnershipAnalysis.h"
#include "dialect/SammineDialect.h"

#include "mlir/Analysis/CFGLoopInfo.h"
#include "mlir/IR/Dominance.h"

#include "llvm/ADT/DenseSet.h"

#include <memory>
#include <set>

//! \file OwnershipCheck.cpp
//! \brief The ownership check: runs the ownership analysis over each MIR
//! function and reports what it finds wrong, each mistake once, in the
//! words the source uses.

namespace sammine_lang {

namespace {

using namespace ownership;

// --- Words ----------------------------------------------------------------

/// Why `op` takes the values it is given.
llvm::StringRef reasonFor(mlir::Operation *op) {
  // A call written as a cast converts what it is given.
  if (auto call = mlir::dyn_cast<smn::CallOp>(op);
      call && call.getWritten() == "as")
    return "converted";
  if (mlir::isa<smn::CallOp, smn::CallIndirectOp, smn::VariantOp>(op))
    return "passed to function";
  if (isImplicitDrop(op))
    return "dropped where it went out of use";
  if (mlir::isa<smn::DropOp>(op))
    return "dropped";
  if (mlir::isa<mlir::func::ReturnOp>(op))
    return "returned";
  if (mlir::isa<smn::ClosureOp>(op))
    return "bound into a closure";
  if (mlir::isa<smn::TagOp>(op))
    return "matched in case";
  if (auto aggregate = mlir::dyn_cast<smn::AggregateOp>(op)) {
    if (mlir::isa<smn::StructType>(aggregate.getType()))
      return "moved into struct";
    if (mlir::isa<smn::TupleType>(aggregate.getType()))
      return "moved into tuple";
    return "moved into array";
  }
  // What a move out of a place takes goes where its one use takes it.
  if (auto move = mlir::dyn_cast<smn::MoveOp>(op);
      move && move.getResult().hasOneUse())
    return reasonFor(*move.getResult().getUsers().begin());
  return "moved";
}

/// What `op` does with a value it uses: takes it, or reaches through it.
llvm::StringRef verbFor(mlir::Operation *op, bool takes) {
  if (takes)
    return "use";
  if (mlir::isa<smn::AddrOp>(op))
    return "take the address of";
  if (!mlir::isa<smn::DerefOp, smn::ThroughOp>(op))
    return "use";
  // Reaching what a pointer points at: for what?
  auto place = op->getResult(0);
  if (!place.hasOneUse())
    return "dereference";
  auto *user = *place.getUsers().begin();
  if (mlir::isa<smn::PtrElemOp>(user))
    return "index";
  if (mlir::isa<smn::LendOp, smn::AddrOp>(user))
    return "use";
  return "dereference";
}

/// How `op` hands on what it takes out of the analysis' sight.
llvm::StringRef handedOnAs(mlir::Operation *op) {
  if (mlir::isa<mlir::func::ReturnOp>(op))
    return "returned";
  if (mlir::isa<smn::ClosureOp>(op))
    return "bound into a closure";
  if (mlir::isa<smn::StoreOp>(op))
    return "stored";
  return "passed to a function";
}

std::string quoted(const Path &path) { return "'" + nameOf(path) + "'"; }

// --- Reporting ------------------------------------------------------------

/// Reports what the analysis tells it, each mistake once: nothing more
/// about a value reported already, nor any part of it, as what a freed
/// pointer pointed at went with it.
class Reporter : public OwnershipObserver {
public:
  explicit Reporter(mlir::func::FuncOp func) : func_(func) {}

  bool failed() const { return failed_; }

  void usedWhileGone(const Path &path, const Owner &owner, mlir::Operation *op,
                     bool takes) override {
    if (!first(path))
      return;
    auto name = quoted(path);
    auto prefix = "Cannot " + verbFor(op, takes).str() + " " + name + " — ";
    if (owner.neverHeld()) {
      error(op->getLoc(),
            prefix + "no owned value is known to be stored there");
      return;
    }
    auto why = reasonFor(owner.movedBy).str();
    std::string when;
    bool loop = false;
    if (!onSomePaths(owner)) {
      when = "it was already " + why + " previously";
    } else if (onOneLoop(owner.movedBy, op)) {
      loop = true;
      when = "it was " + why + " in an earlier turn of the loop";
    } else {
      when = "it was " + why + " in some branches";
    }
    auto diag = error(op->getLoc(), prefix + when);
    // Moved by this very op on an earlier turn: the error shows where.
    if (!loop || owner.movedBy != op)
      diag.attachNote(owner.movedBy->getLoc())
          << name << " was " << why << " here";
  }

  void partGone(const Path &whole, const Path &part, const Owner &owner,
                mlir::Operation *op) override {
    auto why = reasonFor(owner.movedBy).str();
    error(op->getLoc(), "Cannot move " + quoted(whole) + " — its part " +
                            quoted(part) + " was already " + why)
            .attachNote(owner.movedBy->getLoc())
        << quoted(part) << " was " << why << " here";
  }

  void handedOnEmpty(const Path &pointer, const Path &pointee,
                     const Owner &owner, mlir::Operation *op) override {
    if (!first(pointer))
      return;
    auto name = nameOf(pointer);
    auto subject =
        name.empty() ? std::string("An owned pointer") : "'" + name + "'";
    auto msg = subject + " must point at a value when it is " +
               handedOnAs(op).str() + ", but ";
    if (owner.neverHeld()) {
      auto holds = name.empty() ? std::string("it points at")
                                : quoted(pointee) + " holds";
      error(op->getLoc(),
            msg + holds +
                // Held on a path and dropped where the paths join, it held
                // a value in some branches.
                (owner.isEmpty() && !owner.droppedOnSomePath
                     ? " nothing yet"
                     : " a value in some branches only"));
      return;
    }
    auto why = reasonFor(owner.movedBy).str();
    error(op->getLoc(),
          msg + quoted(pointee) + " was " + why +
              (onSomePaths(owner) ? " in some branches" : ""))
            .attachNote(owner.movedBy->getLoc())
        << quoted(pointee) << " was " << why << " here";
  }

  void stillOwned(const Path &whole, llvm::ArrayRef<Path> live,
                  const Owners &owners, mlir::Operation *at) override {
    // A value no binding names is reported once, as a whole, where it is
    // made; one the function returns with is, by name, part by part.
    if (mlir::isa<mlir::func::ReturnOp>(at) && !bindingName(whole.root)) {
      leak(whole, owners);
      return;
    }
    for (auto &path : live)
      leak(path, owners);
  }

  void overwritten(const Path &target, mlir::Operation *store) override {
    error(store->getLoc(),
          "Storing into '" + nameOf(target) + "' while it still owns a value");
  }

  void movedOutOfArray(const Path &element, mlir::Location at) override {
    if (!first(element))
      return;
    auto array = element;
    while (array.steps.back().kind != Step::Element)
      array.steps.pop_back();
    array.steps.pop_back();
    error(at, "Cannot move an element out of " + quoted(array) +
                  ": an array of values that own memory moves "
                  "and drops whole");
  }

  void movedOutOfDropValue(const Path &part, const Path &whole,
                           mlir::Location at) override {
    if (!first(part))
      return;
    llvm::StringRef type;
    if (auto st = mlir::dyn_cast<smn::StructType>(typeOf(whole)))
      type = st.getName();
    else if (auto en = mlir::dyn_cast<smn::EnumType>(typeOf(whole)))
      type = en.getName();
    error(at, "Cannot move " + quoted(part) + " out of " + quoted(whole) +
                  ": '" + type.str() +
                  "' has a Drop instance, so it drops whole; take it apart "
                  "in an unsafe function");
  }

  void lentAndMoved(const Path &lent, mlir::Operation *call) override {
    error(call->getLoc(),
          "Cannot lend " + quoted(lent) + " and move it in the same call");
  }

private:
  mlir::func::FuncOp func_;
  bool failed_ = false;
  std::set<Path> reported_;
  std::unique_ptr<mlir::DominanceInfo> dominance_;
  std::unique_ptr<mlir::CFGLoopInfo> loops_;

  /// Whether `owner` was moved on some paths here only, as the program
  /// reads: moved by it on some, and on the others not, or only dropped
  /// where it went out of use.
  static bool onSomePaths(const Owner &owner) {
    return owner.kind == Owner::MaybeMoved ||
           (owner.droppedOnSomePath && !isImplicitDrop(owner.movedBy));
  }

  mlir::InFlightDiagnostic error(mlir::Location at, const std::string &msg) {
    failed_ = true;
    return mlir::emitError(at, msg);
  }

  /// Whether nothing about `path`, or what holds it, was reported yet.
  bool first(const Path &path) {
    if (llvm::any_of(reported_,
                     [&](const Path &done) { return done.contains(path); }))
      return false;
    reported_.insert(path);
    return true;
  }

  /// Whether one loop holds both `a` and `b`, so control can go from one to
  /// the other and back.
  bool onOneLoop(mlir::Operation *a, mlir::Operation *b) {
    if (!loops_) {
      dominance_ = std::make_unique<mlir::DominanceInfo>(func_);
      loops_ = std::make_unique<mlir::CFGLoopInfo>(
          dominance_->getDomTree(&func_.getBody()));
    }
    for (auto *loop = loops_->getLoopFor(a->getBlock()); loop;
         loop = loop->getParentLoop())
      if (loop->contains(b->getBlock()))
        return true;
    return false;
  }

  void leak(const Path &path, const Owners &owners) {
    if (!first(path))
      return;
    auto &owner = owners.at(path);
    auto name = nameOf(path);
    if (name.empty()) {
      // A value nothing took has no owner. The error underlines it; its type
      // isn't spelled, as MIR's integers carry no sign to spell u32 by.
      error(definedAt(path.root), "Temporary owning memory must not be "
                                  "discarded (bind it with 'let' first)");
    } else if (owner.isPartlyFilled()) {
      error(owner.definedAt, "'" + name +
                                 "' is still owned where it goes out of use: "
                                 "it is stored in some branches");
    } else if (owner.kind == Owner::MaybeMoved) {
      auto why = reasonFor(owner.movedBy).str();
      error(owner.definedAt, "'" + name +
                                 "' is moved in some branches and still "
                                 "owned in others")
              .attachNote(owner.movedBy->getLoc())
          << "'" << name << "' was " << why << " here";
    } else {
      error(owner.definedAt,
            "'" + name + "' is still owned where it goes out of use");
    }
  }
};

// --- The check ------------------------------------------------------------

/// Whether checking `func` found anything wrong: the analysis' state before
/// each op it reaches, applied again, this time with the reporter told.
bool checkFunction(mlir::func::FuncOp func) {
  OwnershipFlow flow(func);
  if (mlir::failed(flow.run())) {
    func.emitError("the ownership check could not analyze this function");
    return true;
  }
  Reporter reporter(func);
  func.walk([&](mlir::Operation *op) {
    if (!flow.reaches(op->getBlock()))
      return;
    auto *before = flow.before(op);
    if (!before)
      return;
    auto owners = *before;
    if (auto branch = mlir::dyn_cast<mlir::BranchOpInterface>(op)) {
      // Each value a branch passes is checked once, on its first edge.
      llvm::DenseSet<mlir::Value> checked;
      for (unsigned i = 0; i < branch->getNumSuccessors(); ++i) {
        auto edge = owners;
        bool fresh = llvm::any_of(
            branch.getSuccessorOperands(i).getForwardedOperands(),
            [&](mlir::Value v) { return checked.insert(v).second; });
        applyEdge(edge, branch, i, fresh ? &reporter : nullptr);
      }
      return;
    }
    applyOp(owners, op, &reporter);
  });
  return reporter.failed();
}

struct CheckOwnershipPass
    : public mlir::PassWrapper<CheckOwnershipPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckOwnershipPass)

  llvm::StringRef getArgument() const override {
    return "sammine-check-ownership";
  }
  llvm::StringRef getDescription() const override {
    return "Check that each value owning memory is used once on every path, "
           "and none is still owned when its function returns";
  }
  void runOnOperation() override {
    if (mlir::failed(checkOwnership(getOperation())))
      signalPassFailure();
  }
};

} // namespace

mlir::LogicalResult checkOwnership(mlir::ModuleOp module) {
  bool failed = false;
  for (auto func : module.getOps<mlir::func::FuncOp>())
    if (!func.isDeclaration() && func->hasAttr(smn::kMirAttr))
      failed |= checkFunction(func);
  return mlir::failure(failed);
}

std::unique_ptr<mlir::Pass> createCheckOwnershipPass() {
  return std::make_unique<CheckOwnershipPass>();
}

void registerCheckOwnershipPass() {
  mlir::PassRegistration<CheckOwnershipPass>();
}

} // namespace sammine_lang
