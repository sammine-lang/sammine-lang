#include "dialect/OwnershipCheck.h"
#include "dialect/SammineDialect.h"

#include "mlir/Analysis/DataFlow/ConstantPropagationAnalysis.h"
#include "mlir/Analysis/DataFlow/DeadCodeAnalysis.h"
#include "mlir/Analysis/DataFlow/DenseAnalysis.h"
#include "mlir/Analysis/DataFlow/SparseAnalysis.h"
#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <string>

//! \file OwnershipCheck.cpp
//! \brief A forward dataflow analysis over MIR, on MLIR's dataflow
//! framework: at each point, which values that own memory are still owned
//! (live) and which were moved, on every path or on some. A walk over the
//! result then reports each use of a moved value and each value still owned
//! where its function returns.

namespace sammine_lang {

namespace {

namespace df = mlir::dataflow;

/// What a value that owns memory is at a point.
struct Owner {
  enum Kind : uint8_t {
    /// It still owns its memory.
    Live,
    /// It was moved on every path here.
    Moved,
    /// It was moved on some paths here only.
    MaybeMoved,
  };
  Kind kind = Live;
  /// Where it was moved, and why: "freed", "passed to function".
  mlir::LocationAttr movedAt;
  llvm::StringRef reason;

  static Owner live() { return {}; }
  static Owner moved(mlir::Location at, llvm::StringRef reason) {
    return {Moved, at, reason};
  }
  bool operator==(const Owner &other) const {
    return kind == other.kind && movedAt == other.movedAt &&
           reason == other.reason;
  }
};

/// Where two paths meet: moved on one and not the other is moved on some.
Owner join(const Owner &a, const Owner &b) {
  if (a.kind == b.kind)
    return a;
  const Owner &moved = a.kind == Owner::Live ? b : a;
  return {Owner::MaybeMoved, moved.movedAt, moved.reason};
}

using Owners = llvm::DenseMap<mlir::Value, Owner>;

/// The values that own memory at a point, and what each is. A value with no
/// entry isn't defined on any path here yet.
class OwnershipState : public df::AbstractDenseLattice {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(OwnershipState)
  using AbstractDenseLattice::AbstractDenseLattice;

  const Owners &owners() const { return owners_; }

  mlir::ChangeResult join(const df::AbstractDenseLattice &rhs) override {
    return join(static_cast<const OwnershipState &>(rhs).owners_);
  }
  mlir::ChangeResult join(const Owners &other) {
    auto result = mlir::ChangeResult::NoChange;
    for (auto &[value, owner] : other) {
      auto [it, inserted] = owners_.try_emplace(value, owner);
      if (inserted) {
        result = mlir::ChangeResult::Change;
        continue;
      }
      auto joined = sammine_lang::join(it->second, owner);
      if (!(joined == it->second)) {
        it->second = joined;
        result = mlir::ChangeResult::Change;
      }
    }
    return result;
  }
  /// Replaces the state with `owners`: what an op's transfer computes from
  /// the state before it.
  mlir::ChangeResult assign(Owners owners) {
    if (owners.size() == owners_.size() &&
        llvm::all_of(owners, [&](auto &entry) {
          auto it = owners_.find(entry.first);
          return it != owners_.end() && it->second == entry.second;
        }))
      return mlir::ChangeResult::NoChange;
    owners_ = std::move(owners);
    return mlir::ChangeResult::Change;
  }

  void print(llvm::raw_ostream &os) const override {
    for (auto &[value, owner] : owners_) {
      value.printAsOperand(os, mlir::OpPrintingFlags());
      static const char *kinds[] = {"live", "moved", "maybe moved"};
      os << ": " << kinds[owner.kind] << "\n";
    }
  }

private:
  Owners owners_;
};

bool ownsMemory(mlir::Value value) { return smn::ownsMemory(value.getType()); }

/// Ops that look at a value that owns memory without taking it: the place
/// a pointer points at, its address; a part of a struct, tuple, enum or
/// array, which the check doesn't follow yet.
bool onlyLooks(mlir::Operation *op) {
  return mlir::isa<smn::DerefOp, smn::AddressOp, smn::TagOp, smn::ExtractOp,
                   smn::PayloadOp, smn::ElementOp>(op);
}

/// Why `op` takes the values it is given.
llvm::StringRef moveReason(mlir::Operation *op) {
  if (mlir::isa<smn::CallOp, smn::CallIndirectOp, smn::VariantOp>(op))
    return "passed to function";
  if (mlir::isa<smn::FreeOp>(op))
    return "freed";
  if (mlir::isa<mlir::func::ReturnOp>(op))
    return "returned";
  if (mlir::isa<smn::ClosureOp>(op))
    return "bound into a closure";
  if (auto aggregate = mlir::dyn_cast<smn::AggregateOp>(op)) {
    auto type = aggregate.getType();
    if (mlir::isa<smn::StructType>(type))
      return "moved into struct";
    if (mlir::isa<smn::TupleType>(type))
      return "moved into tuple";
    return "moved into array";
  }
  return "moved";
}

/// A function argument or block argument that owns memory is live where its
/// block starts.
void addLiveArguments(mlir::Block *block, Owners &owners) {
  for (auto arg : block->getArguments())
    if (ownsMemory(arg))
      owners[arg] = Owner::live();
}

/// Every constant unknown: so DeadCodeAnalysis, which reads branch operands'
/// constants, takes every successor of every branch as reachable. Without
/// some constant analysis it takes none, and with constant propagation it
/// would skip `if false { ... }`, which the check must not depend on.
class UnknownConstants
    : public df::SparseForwardDataFlowAnalysis<df::Lattice<df::ConstantValue>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(UnknownConstants)
  using SparseForwardDataFlowAnalysis::SparseForwardDataFlowAnalysis;

  mlir::LogicalResult visitOperation(
      mlir::Operation *op,
      llvm::ArrayRef<const df::Lattice<df::ConstantValue> *>,
      llvm::ArrayRef<df::Lattice<df::ConstantValue> *> results) override {
    setAllToEntryStates(results);
    return mlir::success();
  }
  void setToEntryState(df::Lattice<df::ConstantValue> *lattice) override {
    propagateIfChanged(lattice,
                       lattice->join(df::ConstantValue::getUnknownConstant()));
  }
};

class OwnershipAnalysis
    : public df::DenseForwardDataFlowAnalysis<OwnershipState> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(OwnershipAnalysis)
  using DenseForwardDataFlowAnalysis::DenseForwardDataFlowAnalysis;

  mlir::LogicalResult visitOperation(mlir::Operation *op,
                                     const OwnershipState &before,
                                     OwnershipState *after) override {
    auto owners = before.owners();
    // A branch moves what it passes along each edge (visitBlockTransfer).
    if (!mlir::isa<mlir::BranchOpInterface>(op) && !onlyLooks(op))
      for (auto operand : op->getOperands())
        if (ownsMemory(operand))
          owners[operand] = Owner::moved(op->getLoc(), moveReason(op));
    for (auto result : op->getResults())
      if (ownsMemory(result))
        owners[result] = Owner::live();
    propagateIfChanged(after, after->assign(std::move(owners)));
    return mlir::success();
  }

  // Along an edge, the branch's operands for it move into the block's
  // arguments.
  void visitBlockTransfer(mlir::Block *block, mlir::ProgramPoint *point,
                          mlir::Block *predecessor,
                          const OwnershipState &before,
                          OwnershipState *after) override {
    auto owners = before.owners();
    auto *terminator = predecessor->getTerminator();
    if (auto branch = mlir::dyn_cast<mlir::BranchOpInterface>(terminator))
      for (unsigned i = 0; i < branch->getNumSuccessors(); ++i) {
        if (branch->getSuccessor(i) != block)
          continue;
        auto forwarded = branch.getSuccessorOperands(i).getForwardedOperands();
        for (auto operand : forwarded)
          if (ownsMemory(operand))
            owners[operand] = Owner::moved(terminator->getLoc(), "moved");
      }
    addLiveArguments(block, owners);
    propagateIfChanged(after, after->join(owners));
  }

  void setToEntryState(OwnershipState *lattice) override {
    Owners owners;
    if (auto *point = llvm::dyn_cast_if_present<mlir::ProgramPoint *>(
            lattice->getAnchor());
        point && point->isBlockStart())
      addLiveArguments(point->getBlock(), owners);
    propagateIfChanged(lattice, lattice->join(owners));
  }
};

/// Reports the errors in one function, from the analysis' result.
class Checker {
public:
  Checker(mlir::func::FuncOp func, mlir::DataFlowSolver &solver)
      : func_(func), solver_(solver) {}

  /// Whether any error was reported.
  bool check() {
    func_.walk([&](mlir::Operation *op) {
      auto *state = stateBefore(op);
      if (!state)
        return;
      checkUses(op, *state);
      if (mlir::isa<mlir::func::ReturnOp>(op))
        checkLeaks(op, *state);
    });
    return failed_;
  }

private:
  mlir::func::FuncOp func_;
  mlir::DataFlowSolver &solver_;
  bool failed_ = false;
  /// Values already reported, once each: one moved twice isn't reported
  /// again as still owned.
  llvm::DenseSet<mlir::Value> reported_;

  /// The state before `op`, or null where no path reaches it.
  const OwnershipState *stateBefore(mlir::Operation *op) {
    auto *executable = solver_.lookupState<df::Executable>(
        solver_.getProgramPointBefore(op->getBlock()));
    if (!executable || !executable->isLive())
      return nullptr;
    return solver_.lookupState<OwnershipState>(
        solver_.getProgramPointBefore(op));
  }

  /// A binding's name, quoted; a value no binding names, by its type.
  static std::string describe(mlir::Value value) {
    if (auto let = value.getDefiningOp<smn::LetOp>())
      return "'" + let.getName().str() + "'";
    return "a temporary of type '" + smn::spelling(value.getType()) + "'";
  }

  static llvm::StringRef verb(mlir::Operation *op) {
    if (mlir::isa<smn::DerefOp>(op))
      return "dereference";
    if (onlyLooks(op))
      return "use";
    return "use linear variable";
  }

  mlir::InFlightDiagnostic error(mlir::Location at, const std::string &msg) {
    failed_ = true;
    return mlir::emitError(at, msg);
  }

  // A value moved before, on every path or on some; or given twice to one op.
  void checkUses(mlir::Operation *op, const OwnershipState &state) {
    llvm::SmallDenseSet<mlir::Value, 4> taken;
    for (auto operand : op->getOperands()) {
      if (!ownsMemory(operand))
        continue;
      auto it = state.owners().find(operand);
      auto name = describe(operand);
      if (it != state.owners().end() && it->second.kind != Owner::Live) {
        auto &owner = it->second;
        reported_.insert(operand);
        auto earlier = owner.kind == Owner::Moved ? "already " : "";
        auto when = owner.movedAt == op->getLoc()
                        ? " in an earlier turn of the loop"
                    : owner.kind == Owner::MaybeMoved ? " on some paths"
                                                      : " previously";
        error(op->getLoc(), "Cannot " + verb(op).str() + " " + name +
                                " — it was " + earlier + owner.reason.str() +
                                when)
                .attachNote(mlir::Location(owner.movedAt))
            << name << " was " << owner.reason << " here";
        continue;
      }
      if (!onlyLooks(op) && !taken.insert(operand).second)
        error(op->getLoc(), "Cannot " + verb(op).str() + " " + name +
                                " twice in one " +
                                (mlir::isa<smn::CallOp>(op) ? "call" : "use"));
    }
  }

  static mlir::Location definedAt(mlir::Value value) {
    if (auto *op = value.getDefiningOp())
      return op->getLoc();
    return mlir::cast<mlir::BlockArgument>(value).getLoc();
  }

  // What the function still owns where it returns, other than what it
  // returns.
  void checkLeaks(mlir::Operation *ret, const OwnershipState &state) {
    llvm::SmallVector<std::pair<mlir::Value, Owner>> owned;
    for (auto &[value, owner] : state.owners())
      if (owner.kind != Owner::Moved &&
          !llvm::is_contained(ret->getOperands(), value) &&
          !reported_.contains(value))
        owned.push_back({value, owner});
    // In the order the values are defined.
    llvm::sort(owned, [&](auto &a, auto &b) {
      return order(a.first) < order(b.first);
    });
    for (auto &[value, owner] : owned) {
      reported_.insert(value);
      auto let = value.getDefiningOp<smn::LetOp>();
      if (owner.kind == Owner::MaybeMoved) {
        error(definedAt(value), "Linear variable " + describe(value) +
                                    " is consumed on some paths but not others")
                .attachNote(mlir::Location(owner.movedAt))
            << describe(value) << " was " << owner.reason << " here";
      } else if (let) {
        error(definedAt(value), "Linear variable " + describe(value) +
                                    " must be consumed before scope exit (use "
                                    "free() to deallocate)");
      } else {
        error(definedAt(value), "Temporary of linear type '" +
                                    smn::spelling(value.getType()) +
                                    "' must not be discarded (bind it with "
                                    "'let' first)");
      }
    }
  }

  /// Where `value` is defined among the function's ops and arguments.
  unsigned order(mlir::Value value) {
    if (positions_.empty()) {
      unsigned n = 0;
      func_.walk([&](mlir::Block *block) {
        for (auto arg : block->getArguments())
          positions_[arg] = n++;
        for (auto &op : *block)
          for (auto result : op.getResults())
            positions_[result] = n++;
      });
    }
    return positions_.lookup(value);
  }
  llvm::DenseMap<mlir::Value, unsigned> positions_;
};

bool checkFunction(mlir::func::FuncOp func) {
  mlir::DataFlowSolver solver(mlir::DataFlowConfig().setInterprocedural(false));
  solver.load<df::DeadCodeAnalysis>();
  solver.load<UnknownConstants>();
  solver.load<OwnershipAnalysis>();
  if (mlir::failed(solver.initializeAndRun(func))) {
    func.emitError("the ownership check could not analyze this function");
    return true;
  }
  return Checker(func, solver).check();
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
