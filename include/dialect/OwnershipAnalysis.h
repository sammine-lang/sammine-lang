#pragma once

#include "dialect/OwnershipPaths.h"

#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"

#include <map>

//! \file OwnershipAnalysis.h
//! \brief A forward dataflow analysis over MIR, on MLIR's dataflow
//! framework: whether each part of each value and place that owns memory
//! is live, moved (on every path or some), or holds nothing yet. What it
//! finds wrong it tells an OwnershipObserver; the ownership check is the
//! one that reports.

namespace sammine_lang::ownership {

/// What a part that owns memory is at a point.
struct Owner {
  enum Kind : uint8_t {
    /// It owns its memory.
    Live,
    /// Moved on every path here; moved by no op, it never held anything
    /// (what alloc gives points at nothing yet).
    Moved,
    /// Moved on some paths here only; moved by no op, it holds a value on
    /// some paths only (stored in some branches).
    MaybeMoved,
  };
  Kind kind = Live;
  /// Where it got what it holds: its binding, or the store that filled it.
  mlir::LocationAttr definedAt;
  /// The op that moved it, if one did. Where paths join, the program's
  /// own move rather than an implicit drop.
  mlir::Operation *movedBy = nullptr;
  /// Whether some path here ended it only with an implicit drop, one the
  /// compiler inserted where it went out of use.
  bool droppedOnSomePath = false;

  static Owner live(mlir::Location at) { return {Live, at, nullptr, false}; }
  /// Holding nothing: what an owned pointer just made points at.
  static Owner empty(mlir::Location at) { return {Moved, at, nullptr, false}; }
  /// Whether it holds nothing on every path here.
  bool isEmpty() const { return kind == Moved && !movedBy; }
  /// Whether it holds a value on some paths only, never having held one on
  /// the others.
  bool isPartlyFilled() const { return kind == MaybeMoved && !movedBy; }
  /// Whether it never held a value on some path: empty, or partly filled.
  bool neverHeld() const { return kind != Live && !movedBy; }
  bool operator==(const Owner &) const = default;
};

/// Whether `op` is a drop the compiler inserted, not one the program wrote,
/// or what takes the part such a drop drops out of what holds it.
bool isImplicitDrop(mlir::Operation *op);

/// Each tracked part at a point. A part with no entry isn't defined on any
/// path here, or isn't one: another variant's payload, once a case knows
/// which variant it has.
using Owners = std::map<Path, Owner>;

/// Every owner in `path`'s tree still holding memory in `owners`, or
/// holding it on some paths only. An owned pointer stands for what it
/// points at.
void liveOwners(const Owners &owners, const Path &path,
                llvm::SmallVectorImpl<Path> &out);

/// What the analysis finds wrong, as it applies an op: told to whoever
/// reports it. Each is told with the state before the op.
class OwnershipObserver {
public:
  virtual ~OwnershipObserver() = default;
  /// `op` uses `path`, which isn't live: moved, on every path or some, or
  /// never holding anything. `takes` if `op` takes it, not only looks.
  virtual void usedWhileGone(const Path &path, const Owner &owner,
                             mlir::Operation *op, bool takes) = 0;
  /// `op` takes `whole`, but its `part` is gone.
  virtual void partGone(const Path &whole, const Path &part, const Owner &owner,
                        mlir::Operation *op) = 0;
  /// `op` hands on the 'ptr `pointer`, out of the analysis' sight, while
  /// what it points at, `pointee`, isn't live.
  virtual void handedOnEmpty(const Path &pointer, const Path &pointee,
                             const Owner &owner, mlir::Operation *op) = 0;
  /// At `at`, a return or the free of what holds them, the parts `live` of
  /// `whole` still own memory: they would be lost.
  virtual void stillOwned(const Path &whole, llvm::ArrayRef<Path> live,
                          const Owners &owners, mlir::Operation *at) = 0;
  /// `store` writes over `target`, which owns memory on some path.
  virtual void overwritten(const Path &target, mlir::Operation *store) = 0;
  /// `call` lends `lent` and takes it too.
  virtual void lentAndMoved(const Path &lent, mlir::Operation *call) = 0;
  /// `element`, a part of an array's elements, read at `at`, is taken: an
  /// array of values that own memory moves and drops whole, as the analysis
  /// follows its elements as one.
  virtual void movedOutOfArray(const Path &element, mlir::Location at) = 0;
  /// `part` of `whole`, whose type has a Drop instance, read at `at`, is
  /// taken outside an unsafe function: such a value drops whole, through
  /// its instance, which counts on all of it.
  virtual void movedOutOfDropValue(const Path &part, const Path &whole,
                                   mlir::Location at) = 0;
};

/// Applies `op` to `owners`, telling `observer`, if any, what it finds.
void applyOp(Owners &owners, mlir::Operation *op, OwnershipObserver *observer);
/// Applies the edge into successor `index` of `branch`: what it passes
/// moves into the block's arguments, and a switch on an enum's tag leaves
/// only the payloads of the variants the edge is for.
void applyEdge(Owners &owners, mlir::BranchOpInterface branch, unsigned index,
               OwnershipObserver *observer);

/// The analysis run on a function: the state before each op it reaches.
class OwnershipFlow {
public:
  explicit OwnershipFlow(mlir::func::FuncOp func);
  /// Runs the solver; failure if it couldn't.
  mlir::LogicalResult run();
  /// Whether some path from the entry reaches `block`. (Not const: the
  /// solver makes a program point the first time it is asked for one.)
  bool reaches(mlir::Block *block);
  /// The state before `op`, or null if nothing reaches it.
  const Owners *before(mlir::Operation *op);

private:
  mlir::func::FuncOp func_;
  mlir::DataFlowSolver solver_;
};

} // namespace sammine_lang::ownership
