#include "dialect/OwnershipAnalysis.h"
#include "dialect/SammineDialect.h"

#include "mlir/Analysis/DataFlow/ConstantPropagationAnalysis.h"
#include "mlir/Analysis/DataFlow/DeadCodeAnalysis.h"
#include "mlir/Analysis/DataFlow/DenseAnalysis.h"
#include "mlir/Analysis/DataFlow/SparseAnalysis.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"

#include "llvm/ADT/DenseSet.h"

//! \file OwnershipAnalysis.cpp
//! \brief The ownership analysis: its lattice, its transfer function, and
//! the solver that runs them over a function.

namespace sammine_lang::ownership {

namespace df = mlir::dataflow;

namespace {

// --- The lattice ----------------------------------------------------------

/// Whether `a` starts before `b` in the source. (See
/// upstream/mlir-location-source-order.)
bool before(mlir::Location a, mlir::Location b) {
  auto ra = a->findInstanceOf<mlir::FileLineColRange>();
  auto rb = b->findInstanceOf<mlir::FileLineColRange>();
  if (!ra || !rb)
    return false;
  return std::pair(ra.getStartLine(), ra.getStartColumn()) <
         std::pair(rb.getStartLine(), rb.getStartColumn());
}

/// Where two paths meet: moved on one and not the other is moved on some.
/// Moved on both, it keeps the program's move over an implicit drop, and
/// else the move that comes first in the source, which a message points
/// at; that some path only dropped it, it keeps too.
Owner join(const Owner &a, const Owner &b) {
  if (a.kind == Owner::Live && b.kind == Owner::Live)
    return a;
  if (a.kind == Owner::Live || b.kind == Owner::Live) {
    auto &live = a.kind == Owner::Live ? a : b;
    Owner moved = a.kind == Owner::Live ? b : a;
    moved.kind = Owner::MaybeMoved;
    // Filled on one path only: where it was filled is what to show.
    if (!moved.movedBy)
      moved.definedAt = live.definedAt;
    return moved;
  }
  Owner first = a;
  bool aDrop = a.movedBy && isImplicitDrop(a.movedBy);
  bool bDrop = b.movedBy && isImplicitDrop(b.movedBy);
  if (aDrop != bDrop)
    first = aDrop ? b : a;
  else if (a.movedBy && b.movedBy &&
           before(b.movedBy->getLoc(), a.movedBy->getLoc()))
    first = b;
  first.droppedOnSomePath =
      a.droppedOnSomePath || b.droppedOnSomePath || aDrop || bDrop;
  first.kind = a.kind == Owner::Moved && b.kind == Owner::Moved
                   ? Owner::Moved
                   : Owner::MaybeMoved;
  return first;
}

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
    for (auto &[path, owner] : other) {
      auto [it, inserted] = owners_.try_emplace(path, owner);
      if (inserted) {
        result = mlir::ChangeResult::Change;
        continue;
      }
      auto joined = ownership::join(it->second, owner);
      if (!(joined == it->second)) {
        it->second = joined;
        result = mlir::ChangeResult::Change;
      }
    }
    return result;
  }
  /// Replaces the state: what an op computes from the state before it.
  mlir::ChangeResult assign(Owners owners) {
    if (owners == owners_)
      return mlir::ChangeResult::NoChange;
    owners_ = std::move(owners);
    return mlir::ChangeResult::Change;
  }

  void print(llvm::raw_ostream &os) const override {
    static const char *kinds[] = {"live", "moved", "maybe moved"};
    for (auto &[path, owner] : owners_) {
      path.root.printAsOperand(os, mlir::OpPrintingFlags());
      os << " (" << path.steps.size() << " steps): " << kinds[owner.kind]
         << "\n";
    }
  }

private:
  Owners owners_;
};

// --- New values -----------------------------------------------------------

/// `path` and its parts, as a new value of its type has them: all live, as
/// a 'ptr holds a value once it leaves the function that allocated it,
/// unless the value is `empty`, or was `allocated` here, so that what it
/// points at holds nothing yet.
void fresh(Owners &owners, const Path &path, mlir::Location at,
           bool empty = false, bool allocated = false) {
  owners[path] = empty ? Owner::empty(at) : Owner::live(at);
  for (auto step : partsOf(typeOf(path)))
    fresh(owners, path.child(step), at,
          empty || (allocated && step.kind == Step::Pointee));
}

/// `to` holds what `from` held in `before`, part by part; got at `at`, or
/// where `from` got it when `at` is none. A fresh value if `from` held
/// nothing to hand on.
void handOn(const Owners &before, Owners &owners,
            const std::optional<Path> &from, const Path &to,
            std::optional<mlir::Location> at) {
  auto it = from ? before.find(*from) : before.end();
  if (it == before.end() || it->second.kind != Owner::Live) {
    fresh(owners, to, at.value_or(definedAt(to.root)));
    return;
  }
  for (auto &[path, owner] : before) {
    if (!from->contains(path))
      continue;
    Owner copied = owner;
    if (at)
      copied.definedAt = *at;
    owners[from->moveTo(path, to)] = copied;
  }
}

// --- The transfer function ------------------------------------------------

/// Applies one op, or one edge, to the state, telling the observer, if
/// any, what it finds wrong.
class Transfer {
public:
  Transfer(Owners &owners, OwnershipObserver *observer)
      : owners_(owners), observer_(observer) {}

  void apply(mlir::Operation *op);
  void edge(mlir::BranchOpInterface branch, unsigned index);

private:
  Owners &owners_;
  OwnershipObserver *observer_;

  const Owner *lookup(const Path &path) const {
    auto it = owners_.find(path);
    return it == owners_.end() ? nullptr : &it->second;
  }

  /// The nearest path the analysis follows that holds what `value` names:
  /// reading a part that owns nothing reads what it is a part of.
  std::optional<Path> nearestTracked(mlir::Value value) const {
    auto path = pathOf(value);
    if (!path) {
      auto *op = value.getDefiningOp();
      if (!op || op->getNumOperands() == 0 ||
          !mlir::isa<smn::ExtractOp, smn::PayloadOp, smn::ElementOp,
                     smn::FieldOp, smn::ElemOp>(op))
        return std::nullopt;
      return nearestTracked(op->getOperand(0));
    }
    // Up to what holds the part, but not past a pointer or into an enum: the
    // deref that reached it checks the pointer, the case that took the enum
    // apart checks the enum.
    while (!lookup(*path) && !path->steps.empty() &&
           path->steps.back().kind != Step::Pointee &&
           path->steps.back().kind != Step::Payload)
      path->steps.pop_back();
    if (!lookup(*path))
      return std::nullopt;
    return path;
  }

  /// `path` and every part of it are moved by `by`. A part moved before
  /// keeps the op that moved it.
  void markMoved(const Path &path, mlir::Operation *by) {
    for (auto &[part, owner] : owners_) {
      if (!path.contains(part))
        continue;
      if (part == path || owner.kind == Owner::Live || owner.neverHeld()) {
        owner.kind = Owner::Moved;
        owner.movedBy = by;
      }
    }
  }

  /// Whether `path` is live where `op` uses it; tells the observer if not.
  bool checkLive(const Path &path, mlir::Operation *op, bool takes) {
    auto *owner = lookup(path);
    if (!owner || owner->kind == Owner::Live)
      return true;
    if (observer_)
      observer_->usedWhileGone(path, *owner, op, takes);
    return false;
  }

  /// What `op` hands on leaves what the analysis sees: a 'ptr in it must
  /// hold a value, so that whoever gets it can count on one.
  void checkHolds(mlir::Value value, mlir::Operation *op) {
    auto path = pathOf(value);
    if (!observer_ || !path || !lookup(*path) ||
        lookup(*path)->kind != Owner::Live)
      return;
    for (auto &[part, owner] : owners_)
      if (path->contains(part) && part.isPointee() && owner.kind != Owner::Live)
        observer_->handedOnEmpty(part.parent(), part, owner, op);
  }

  /// Moves the whole of what `value` names, for `op`.
  void take(mlir::Value value, mlir::Operation *op) {
    auto path = pathOf(value);
    if (!path || !lookup(*path))
      return;
    if (checkLive(*path, op, /*takes=*/true) && observer_)
      // Its parts must all be there; what an owned pointer points at goes
      // with it whatever it holds.
      for (auto &[part, owner] : owners_) {
        if (!path->contains(part) || part == *path ||
            owner.kind == Owner::Live || owner.neverHeld())
          continue;
        if (llvm::any_of(llvm::drop_begin(part.steps, path->steps.size()),
                         [](Step s) { return s.kind == Step::Pointee; }))
          continue;
        observer_->partGone(*path, part, owner, op);
        break;
      }
    markMoved(*path, op);
  }

  /// Looks at what `value` names, through `op`.
  void look(mlir::Value value, mlir::Operation *op) {
    if (auto path = nearestTracked(value))
      checkLive(*path, op, /*takes=*/false);
  }

  void liveOwners(const Path &path, llvm::SmallVectorImpl<Path> &out) const {
    ownership::liveOwners(owners_, path, out);
  }


  /// Tells the observer what of `whole` is still owned at `at`.
  void checkOwned(const Path &whole, mlir::Operation *at) {
    if (!observer_)
      return;
    llvm::SmallVector<Path> live;
    liveOwners(whole, live);
    if (!live.empty())
      observer_->stillOwned(whole, live, owners_, at);
  }

  /// What is still owned where the function returns, at `ret`.
  void checkLeaks(mlir::Operation *ret) {
    llvm::SmallVector<Path> roots;
    for (auto &[path, owner] : owners_)
      if (path.steps.empty())
        roots.push_back(path);
    for (auto &root : roots)
      checkOwned(root, ret);
  }

  /// A call can't lend what it also takes: the callee could free it while
  /// still reading the view.
  void checkLendAndMove(mlir::Operation *call) {
    if (!observer_)
      return;
    for (auto lent : call->getOperands()) {
      auto lend = lent.getDefiningOp<smn::LendOp>();
      if (!lend)
        continue;
      auto place = pathOf(lend.getPlace());
      if (!place)
        continue;
      for (auto taken : call->getOperands()) {
        auto path = pathOf(taken);
        if (!path || !smn::ownsMemory(taken.getType()) ||
            !path->contains(*place))
          continue;
        // A view of what a pointer points at is a view of the pointer.
        observer_->lentAndMoved(place->isPointee() ? place->parent() : *place,
                                call);
      }
    }
  }
};

void Transfer::apply(mlir::Operation *op) {
  // A branch moves what it passes along each edge (edge()).
  if (mlir::isa<mlir::BranchOpInterface>(op))
    return;
  auto loc = op->getLoc();

  if (auto let = mlir::dyn_cast<smn::LetOp>(op)) {
    Owners before = owners_;
    take(let.getValue(), op);
    handOn(before, owners_, pathOf(let.getValue()), Path{let.getResult(), {}},
           loc);
    return;
  }
  if (auto store = mlir::dyn_cast<smn::StoreOp>(op)) {
    auto value = store.getValue();
    if (!smn::ownsMemory(value.getType()))
      return;
    Owners before = owners_;
    auto target = pathOf(store.getPlace());
    bool tracked = target && lookup(*target);
    // Stored where the analysis doesn't follow, as a buffer's values.
    if (!tracked)
      checkHolds(value, op);
    take(value, op);
    if (!tracked)
      return;
    // A place takes a value only once what it held is gone, on every path.
    // Told, the place holds the new value all the same, so what follows
    // isn't told as well.
    llvm::SmallVector<Path> live;
    liveOwners(*target, live);
    if (!live.empty() && observer_)
      observer_->overwritten(*target, op);
    for (auto &[path, owner] : Owners(owners_))
      if (target->contains(path))
        owners_.erase(path);
    handOn(before, owners_, pathOf(value), *target, loc);
    return;
  }
  if (auto move = mlir::dyn_cast<smn::MoveOp>(op)) {
    // What is moved owns what it took: what the analysis follows of the
    // place, or, from a place it doesn't follow, a value of its own.
    auto place = pathOf(move.getPlace());
    Path result{move.getResult(), {}};
    if (!place || !lookup(*place)) {
      fresh(owners_, result, loc);
      return;
    }
    Owners before = owners_;
    checkLive(*place, op, /*takes=*/true);
    handOn(before, owners_, place, result, std::nullopt);
    markMoved(*place, op);
    return;
  }
  if (auto drop = mlir::dyn_cast<smn::DropOp>(op)) {
    auto path = pathOf(drop.getValue());
    if (path && lookup(*path) && lookup(*path)->kind == Owner::Live &&
        isOwnedPtr(drop.getValue().getType()))
      // Dropping an owner frees only its own memory: what it points at must
      // hold nothing by now. (A struct's drop takes all of it.)
      for (auto step : partsOf(typeOf(*path)))
        checkOwned(path->child(step), op);
    take(drop.getValue(), op);
    return;
  }
  if (auto ret = mlir::dyn_cast<mlir::func::ReturnOp>(op)) {
    for (auto operand : ret.getOperands()) {
      checkHolds(operand, op);
      take(operand, op);
    }
    checkLeaks(op);
    return;
  }
  if (auto tag = mlir::dyn_cast<smn::TagOp>(op)) {
    // A case takes the value apart: its payloads go to the arms' bindings,
    // and the value itself is gone.
    auto path = pathOf(tag.getValue());
    if (!path || !lookup(*path))
      return;
    // One already gone is told here; the arms find its payloads as if it
    // weren't, so nothing more is told.
    if (!checkLive(*path, op, /*takes=*/true))
      fresh(owners_, *path, loc);
    auto &owner = owners_[*path];
    owner.kind = Owner::Moved;
    owner.movedBy = op;
    return;
  }
  if (mlir::isa<smn::DerefOp, smn::ThroughOp>(op)) {
    // Reaching what a pointer points at reads the pointer: a value, or one
    // held in a place.
    auto pointer = op->getOperand(0);
    auto type = pointer.getType();
    if (auto place = mlir::dyn_cast<smn::PlaceType>(type))
      type = place.getValueType();
    if (isOwnedPtr(type))
      look(pointer, op);
    return;
  }
  if (mlir::isa<smn::LendOp, smn::AddrOp>(op)) {
    // A view of what a pointer points at: the deref or through that reached
    // it read the pointer, and the view may see it hold nothing, as the
    // pointer's type allows. A view of any other place reads the place.
    auto place = pathOf(op->getOperand(0));
    if (place && place->isPointee())
      return;
    look(op->getOperand(0), op);
    return;
  }
  if (mlir::isa<smn::PtrElemOp, smn::FieldOp, smn::ElemOp>(op))
    return;
  if (mlir::isa<smn::CopyOp, smn::AddressOp>(op)) {
    look(op->getOperand(0), op);
    return;
  }
  if (mlir::isa<smn::ExtractOp, smn::PayloadOp, smn::ElementOp>(op)) {
    // A part that owns memory is taken where it is used; reading one that
    // owns nothing reads what it is part of.
    if (!smn::ownsMemory(op->getResult(0).getType()))
      look(op->getResult(0), op);
    return;
  }
  if (auto local = mlir::dyn_cast<smn::LocalOp>(op)) {
    // A slot holds nothing until something is stored there.
    if (smn::ownsMemory(local.getPlace().getType().getValueType()))
      fresh(owners_, Path{local.getPlace(), {}}, loc, /*empty=*/true);
    return;
  }

  // Anything else takes what it is given and makes new values: a struct,
  // tuple, array or variant owns what it is built from.
  bool handsOn =
      mlir::isa<smn::CallOp, smn::CallIndirectOp, smn::ClosureOp>(op);
  if (mlir::isa<smn::CallOp, smn::CallIndirectOp>(op))
    checkLendAndMove(op);
  Owners before = owners_;
  for (auto operand : op->getOperands()) {
    if (handsOn)
      checkHolds(operand, op);
    take(operand, op);
  }
  bool builds = mlir::isa<smn::AggregateOp, smn::VariantOp>(op);
  for (auto result : op->getResults()) {
    if (!smn::ownsMemory(result.getType()))
      continue;
    Path path{result, {}};
    if (!builds) {
      fresh(owners_, path, loc, /*empty=*/false,
            /*allocated=*/mlir::isa<smn::AllocOp>(op));
      continue;
    }
    owners_[path] = Owner::live(loc);
    for (auto [i, operand] : llvm::enumerate(op->getOperands())) {
      if (!smn::ownsMemory(operand.getType()))
        continue;
      Step step{Step::Field, static_cast<int64_t>(i)};
      if (auto variant = mlir::dyn_cast<smn::VariantOp>(op))
        step = {Step::Payload, static_cast<int64_t>(variant.getIndex()),
                static_cast<int64_t>(i)};
      else if (mlir::isa<smn::ArrayType>(result.getType()))
        step = {Step::Element};
      handOn(before, owners_, pathOf(operand), path.child(step), loc);
    }
  }
}

void Transfer::edge(mlir::BranchOpInterface branch, unsigned index) {
  auto *op = branch.getOperation();
  auto *block = branch->getSuccessor(index);
  auto forwarded = branch.getSuccessorOperands(index).getForwardedOperands();
  Owners before = owners_;
  for (auto [operand, arg] : llvm::zip(forwarded, block->getArguments())) {
    if (!smn::ownsMemory(arg.getType()))
      continue;
    take(operand, op);
    handOn(before, owners_, pathOf(operand), Path{arg, {}}, arg.getLoc());
  }
  // A switch on an enum's tag: the arm for a variant has only its payload;
  // the default, the payloads of the variants no case names.
  auto sw = mlir::dyn_cast<mlir::cf::SwitchOp>(branch.getOperation());
  if (!sw)
    return;
  auto tag = sw.getFlag().getDefiningOp<smn::TagOp>();
  if (!tag)
    return;
  auto path = pathOf(tag.getValue());
  if (!path)
    return;
  llvm::SmallDenseSet<int64_t> variants;
  auto caseValues = sw.getCaseValues();
  if (index == 0) {
    auto count = mlir::cast<smn::EnumType>(tag.getValue().getType())
                     .getVariants()
                     .size();
    for (size_t v = 0; v < count; ++v)
      variants.insert(static_cast<int64_t>(v));
    if (caseValues)
      for (auto value : caseValues->getValues<llvm::APInt>())
        variants.erase(value.getSExtValue());
  } else if (caseValues) {
    variants.insert(
        (*caseValues).getValues<llvm::APInt>()[index - 1].getSExtValue());
  }
  auto depth = path->steps.size();
  for (auto &[part, owner] : Owners(owners_))
    if (path->contains(part) && part.steps.size() > depth &&
        part.steps[depth].kind == Step::Payload &&
        !variants.contains(part.steps[depth].a))
      owners_.erase(part);
}

// --- The solver -----------------------------------------------------------

/// Every constant unknown: so DeadCodeAnalysis, which reads branch operands'
/// constants, takes every successor of every branch as reachable. Without
/// some constant analysis it takes none, and with constant propagation it
/// would skip `if false { ... }`, which the analysis must not depend on.
/// (See upstream/mlir-deadcode-needs-constant-lattice.)
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
    Transfer(owners, nullptr).apply(op);
    propagateIfChanged(after, after->assign(std::move(owners)));
    return mlir::success();
  }

  void visitBlockTransfer(mlir::Block *block, mlir::ProgramPoint *point,
                          mlir::Block *predecessor,
                          const OwnershipState &before,
                          OwnershipState *after) override {
    auto owners = before.owners();
    if (auto branch = mlir::dyn_cast<mlir::BranchOpInterface>(
            predecessor->getTerminator()))
      for (unsigned i = 0; i < branch->getNumSuccessors(); ++i)
        if (branch->getSuccessor(i) == block)
          Transfer(owners, nullptr).edge(branch, i);
    propagateIfChanged(after, after->join(owners));
  }

  // A function's arguments are live where it starts, and what an owned one
  // points at holds a value: its caller had to put one there.
  void setToEntryState(OwnershipState *lattice) override {
    Owners owners;
    if (auto *point = llvm::dyn_cast_if_present<mlir::ProgramPoint *>(
            lattice->getAnchor());
        point && point->isBlockStart())
      for (auto arg : point->getBlock()->getArguments())
        if (smn::ownsMemory(arg.getType()))
          fresh(owners, Path{arg, {}}, arg.getLoc());
    propagateIfChanged(lattice, lattice->join(owners));
  }
};

} // namespace

void liveOwners(const Owners &owners, const Path &path,
                llvm::SmallVectorImpl<Path> &out) {
  auto it = owners.find(path);
  if (it == owners.end() || it->second.isEmpty())
    return;
  auto &owner = it->second;
  if (owner.kind == Owner::MaybeMoved) {
    out.push_back(path);
    return;
  }
  auto type = typeOf(path);
  auto parts = partsOf(type);
  bool hasParts = llvm::any_of(
      parts, [&](Step s) { return owners.count(path.child(s)) != 0; });
  if (owner.kind == Owner::Live && (isOwnedPtr(type) || !hasParts)) {
    out.push_back(path);
    return;
  }
  for (auto step : parts)
    liveOwners(owners, path.child(step), out);
}

bool isImplicitDrop(mlir::Operation *op) {
  // A part is taken out of what holds it, then dropped.
  if (mlir::isa<smn::MoveOp, smn::ExtractOp>(op) &&
      op->getResult(0).hasOneUse())
    op = *op->getResult(0).getUsers().begin();
  auto drop = mlir::dyn_cast<smn::DropOp>(op);
  return drop && drop.getImplicit();
}

void applyOp(Owners &owners, mlir::Operation *op, OwnershipObserver *observer) {
  Transfer(owners, observer).apply(op);
}

void applyEdge(Owners &owners, mlir::BranchOpInterface branch, unsigned index,
               OwnershipObserver *observer) {
  Transfer(owners, observer).edge(branch, index);
}

OwnershipFlow::OwnershipFlow(mlir::func::FuncOp func)
    : func_(func), solver_(mlir::DataFlowConfig().setInterprocedural(false)) {
  solver_.load<df::DeadCodeAnalysis>();
  solver_.load<UnknownConstants>();
  solver_.load<OwnershipAnalysis>();
}

mlir::LogicalResult OwnershipFlow::run() {
  return solver_.initializeAndRun(func_);
}

bool OwnershipFlow::reaches(mlir::Block *block) {
  auto *executable =
      solver_.lookupState<df::Executable>(solver_.getProgramPointBefore(block));
  return executable && executable->isLive();
}

const Owners *OwnershipFlow::before(mlir::Operation *op) {
  auto *state =
      solver_.lookupState<OwnershipState>(solver_.getProgramPointBefore(op));
  return state ? &state->owners() : nullptr;
}

} // namespace sammine_lang::ownership
