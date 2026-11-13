#include "dialect/OwnershipCheck.h"
#include "dialect/SammineDialect.h"

#include "mlir/Analysis/DataFlow/ConstantPropagationAnalysis.h"
#include "mlir/Analysis/DataFlow/DeadCodeAnalysis.h"
#include "mlir/Analysis/DataFlow/DenseAnalysis.h"
#include "mlir/Analysis/DataFlow/SparseAnalysis.h"
#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <map>
#include <optional>
#include <set>
#include <string>

//! \file OwnershipCheck.cpp
//! \brief A forward dataflow analysis over MIR, on MLIR's dataflow
//! framework. It follows every part of every value and place that owns
//! memory, down to fields, payloads, elements and what an owned pointer
//! points at: whether it is live, moved (on every path or some), or holds
//! nothing yet. One transfer function serves both the analysis and a walk
//! over its result that reports each misuse.

namespace sammine_lang {

namespace {

namespace df = mlir::dataflow;

/// One step from a value or place to a part of it.
struct Step {
  enum Kind : uint8_t {
    /// Field `a` of a struct or tuple.
    Field,
    /// Field `b` of the payload of variant `a` of an enum.
    Payload,
    /// What an owned pointer points at.
    Pointee,
    /// Any element of an array: the check doesn't tell them apart.
    Element,
  };
  Kind kind;
  int64_t a = 0, b = 0;
  auto operator<=>(const Step &) const = default;
};

/// A part of a value that owns memory, or of a place: a root, a value an op
/// makes or a binding's slot, and the steps to the part.
struct Path {
  mlir::Value root;
  llvm::SmallVector<Step, 2> steps;

  Path child(Step step) const {
    Path path = *this;
    path.steps.push_back(step);
    return path;
  }
  /// Whether `other` is this path or a part of it.
  bool contains(const Path &other) const {
    return root == other.root && steps.size() <= other.steps.size() &&
           std::equal(steps.begin(), steps.end(), other.steps.begin());
  }
  /// `other`, a part of this path, as the same part of `to`.
  Path moveTo(const Path &other, const Path &to) const {
    Path moved = to;
    moved.steps.append(other.steps.begin() + steps.size(), other.steps.end());
    return moved;
  }
  bool operator==(const Path &other) const {
    return root == other.root && steps == other.steps;
  }
  bool operator<(const Path &other) const {
    if (root != other.root)
      return root.getAsOpaquePointer() < other.root.getAsOpaquePointer();
    return std::lexicographical_compare(steps.begin(), steps.end(),
                                        other.steps.begin(), other.steps.end());
  }
};

/// What a part that owns memory is at a point.
struct Owner {
  enum Kind : uint8_t {
    /// It owns its memory.
    Live,
    /// It was moved on every path here; with no reason, it never held
    /// anything (what alloc gives points at nothing yet).
    Moved,
    /// Moved on some paths here only.
    MaybeMoved,
  };
  Kind kind = Live;
  /// Where it got what it holds: its binding, or the store that filled it.
  mlir::LocationAttr definedAt;
  /// Where it was moved, by which op, and why ("freed").
  mlir::LocationAttr movedAt;
  mlir::Operation *movedBy = nullptr;
  llvm::StringRef reason;

  static Owner live(mlir::Location at) { return {Live, at, {}, nullptr, {}}; }
  /// Holding nothing: what an owned pointer just made points at.
  static Owner empty(mlir::Location at) { return {Moved, at, {}, nullptr, {}}; }
  bool isEmpty() const { return kind != Live && reason.empty(); }
  bool operator==(const Owner &) const = default;
};

/// Whether `a` comes before `b` in the source.
bool before(mlir::LocationAttr a, mlir::LocationAttr b) {
  auto ra = mlir::dyn_cast_if_present<mlir::FileLineColRange>(a);
  auto rb = mlir::dyn_cast_if_present<mlir::FileLineColRange>(b);
  if (!ra || !rb)
    return false;
  return std::pair(ra.getStartLine(), ra.getStartColumn()) <
         std::pair(rb.getStartLine(), rb.getStartColumn());
}

/// Where two paths meet: moved on one and not the other is moved on some.
/// Moved on both, it keeps the move that comes first in the source, which a
/// message points at.
Owner join(const Owner &a, const Owner &b) {
  if (a.kind == Owner::Live && b.kind == Owner::Live)
    return a;
  if (a.kind == Owner::Live || b.kind == Owner::Live) {
    Owner moved = a.kind == Owner::Live ? b : a;
    moved.kind = Owner::MaybeMoved;
    return moved;
  }
  Owner first = before(b.movedAt, a.movedAt) ? b : a;
  first.kind = a.kind == Owner::Moved && b.kind == Owner::Moved
                   ? Owner::Moved
                   : Owner::MaybeMoved;
  return first;
}

/// Each tracked part at a point. A part with no entry isn't defined on any
/// path here, or isn't one: another variant's payload, once a case knows
/// which variant it has.
using Owners = std::map<Path, Owner>;

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
      auto joined = sammine_lang::join(it->second, owner);
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

// --- Types and paths ------------------------------------------------------

/// The type of what `path` names: a value's, or what a place holds.
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

/// The parts of a `type` that own memory, one step down.
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
    if (ptr.isOwned() && smn::ownsMemory(ptr.getPointee()))
      parts.push_back({Step::Pointee});
  }
  return parts;
}

/// `path` and its parts, as a new value of its type has them: live, but
/// what an owned pointer points at holds nothing yet.
void fresh(Owners &owners, const Path &path, mlir::Location at,
           bool empty = false) {
  owners[path] = empty ? Owner::empty(at) : Owner::live(at);
  for (auto step : partsOf(typeOf(path)))
    fresh(owners, path.child(step), at, empty || step.kind == Step::Pointee);
}

mlir::Location definedAt(mlir::Value value) {
  if (auto *op = value.getDefiningOp())
    return op->getLoc();
  return mlir::cast<mlir::BlockArgument>(value).getLoc();
}

/// Whether every use of `value` only looks at it: dereferences it, indexes
/// past it, takes its address.
bool onlyLookedAt(mlir::Value value) {
  return !value.use_empty() &&
         llvm::all_of(value.getUsers(), [](mlir::Operation *user) {
           return mlir::isa<smn::DerefOp, smn::PtrElemOp, smn::AddressOp>(user);
         });
}

/// The path a value or place names: a value that owns memory, a part of
/// one, a slot, a part of a place. Nullopt for one the check doesn't
/// follow: owning nothing, or behind a pointer that doesn't own.
std::optional<Path> pathOf(mlir::Value value) {
  auto *op = value.getDefiningOp();
  if (auto move = mlir::dyn_cast_if_present<smn::MoveOp>(op)) {
    auto place = pathOf(move.getPlace());
    // What is moved out of a place the function doesn't own, behind a view
    // or past a pointer, is read where it lives: its owner is elsewhere.
    if (!place)
      return std::nullopt;
    // So is one only looked at, as `**pp` reads `*pp` to reach what it
    // points at: it names the place.
    if (onlyLookedAt(move.getResult()))
      return place;
  }
  if (!op ||
      !mlir::isa<smn::ExtractOp, smn::PayloadOp, smn::ElementOp, smn::FieldOp,
                 smn::ElemOp, smn::DerefOp, smn::PtrElemOp, smn::LocalOp>(op)) {
    if (!smn::ownsMemory(value.getType()))
      return std::nullopt;
    return Path{value, {}};
  }
  if (mlir::isa<smn::LocalOp>(op))
    return Path{value, {}};
  if (mlir::isa<smn::PtrElemOp>(op))
    return std::nullopt;
  std::optional<Path> base;
  Step step{};
  if (auto extract = mlir::dyn_cast<smn::ExtractOp>(op)) {
    base = pathOf(extract.getAggregate());
    step = {Step::Field, static_cast<int64_t>(extract.getIndex())};
  } else if (auto payload = mlir::dyn_cast<smn::PayloadOp>(op)) {
    base = pathOf(payload.getValue());
    step = {Step::Payload, static_cast<int64_t>(payload.getVariant()),
            static_cast<int64_t>(payload.getField())};
  } else if (auto element = mlir::dyn_cast<smn::ElementOp>(op)) {
    base = pathOf(element.getArray());
    step = {Step::Element};
  } else if (auto field = mlir::dyn_cast<smn::FieldOp>(op)) {
    base = pathOf(field.getBase());
    step = {Step::Field, static_cast<int64_t>(field.getIndex())};
  } else if (auto elem = mlir::dyn_cast<smn::ElemOp>(op)) {
    base = pathOf(elem.getBase());
    step = {Step::Element};
  } else {
    auto deref = mlir::cast<smn::DerefOp>(op);
    if (!isOwnedPtr(deref.getPtr().getType()))
      return std::nullopt;
    base = pathOf(deref.getPtr());
    step = {Step::Pointee};
  }
  if (!base)
    return std::nullopt;
  return base->child(step);
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

/// A binding's name, for a root a binding makes.
std::string nameOf(const Path &path);

/// A binding's name, for a root a binding makes; a value moved out of a
/// place goes by the place's.
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

/// `path` as the source writes it: `b.data`, `*p.a`, `o.i.a`; empty for a
/// value no binding names.
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

// --- Reporting ------------------------------------------------------------

/// Where a transfer reports what it finds, once the analysis is done.
class Reports {
public:
  mlir::InFlightDiagnostic error(mlir::Location at, const std::string &msg) {
    failed_ = true;
    return mlir::emitError(at, msg);
  }
  bool failed() const { return failed_; }

  /// Whether the blocks of `a` and `b` lie on one cycle, so control can go
  /// from one to the other and back: a loop holding both.
  bool onOneCycle(mlir::Operation *a, mlir::Operation *b) {
    return reaches(a->getBlock(), b->getBlock()) &&
           reaches(b->getBlock(), a->getBlock());
  }

  /// A part reported at a use, or still owned at an earlier return, isn't
  /// reported again as still owned, nor is any part of it.
  void reported(const Path &path) { leaked_.insert(path); }
  bool firstLeak(const Path &path) {
    if (llvm::any_of(leaked_,
                     [&](const Path &done) { return done.contains(path); }))
      return false;
    leaked_.insert(path);
    return true;
  }

private:
  bool failed_ = false;
  std::set<Path> leaked_;
  llvm::DenseMap<mlir::Block *, llvm::DenseSet<mlir::Block *>> reach_;

  /// Whether control can go from `from` to `to` through at least one edge.
  bool reaches(mlir::Block *from, mlir::Block *to) {
    auto it = reach_.find(from);
    if (it == reach_.end()) {
      llvm::DenseSet<mlir::Block *> seen;
      llvm::SmallVector<mlir::Block *> work(from->getSuccessors().begin(),
                                            from->getSuccessors().end());
      while (!work.empty()) {
        auto *block = work.pop_back_val();
        if (!seen.insert(block).second)
          continue;
        work.append(block->getSuccessors().begin(),
                    block->getSuccessors().end());
      }
      it = reach_.try_emplace(from, std::move(seen)).first;
    }
    return it->second.contains(to);
  }
};

// --- The transfer function ------------------------------------------------

/// Why `op` takes the values it is given.
llvm::StringRef reasonFor(mlir::Operation *op) {
  if (auto reason = op->getAttrOfType<mlir::StringAttr>(smn::kReasonAttr))
    return reason.getValue();
  if (mlir::isa<smn::CallOp, smn::CallIndirectOp, smn::VariantOp>(op))
    return "passed to function";
  if (mlir::isa<smn::FreeOp>(op))
    return "freed";
  if (mlir::isa<mlir::func::ReturnOp>(op))
    return "returned";
  if (mlir::isa<smn::ClosureOp>(op))
    return "bound into a closure";
  if (auto aggregate = mlir::dyn_cast<smn::AggregateOp>(op)) {
    if (mlir::isa<smn::StructType>(aggregate.getType()))
      return "moved into struct";
    if (mlir::isa<smn::TupleType>(aggregate.getType()))
      return "moved into tuple";
    return "moved into array";
  }
  return "moved";
}

/// Applies one op, or one edge, to the state; reports what it finds when
/// `reports` is set.
class Transfer {
public:
  Transfer(Owners &owners, Reports *reports)
      : owners_(owners), reports_(reports) {}

  void apply(mlir::Operation *op);
  /// Along the edge into successor `index` of `branch`: what it passes moves
  /// into the block's arguments, and a switch on an enum's tag leaves only
  /// the payloads of the variants the edge is for.
  void edge(mlir::BranchOpInterface branch, unsigned index);

private:
  Owners &owners_;
  Reports *reports_;

  const Owner *lookup(const Path &path) const {
    auto it = owners_.find(path);
    return it == owners_.end() ? nullptr : &it->second;
  }

  /// The nearest path the check follows that holds what `value` names:
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

  /// `path` and every part of it are moved. A part moved before keeps where
  /// that happened.
  void markMoved(const Path &path, mlir::Operation *by, llvm::StringRef why) {
    for (auto &[part, owner] : owners_) {
      if (!path.contains(part))
        continue;
      if (part == path || owner.kind == Owner::Live || owner.isEmpty()) {
        owner.kind = Owner::Moved;
        owner.movedAt = by->getLoc();
        owner.movedBy = by;
        owner.reason = why;
      }
    }
  }

  /// Reports a use of `path` that was moved, on every path or some, or that
  /// never held anything. Returns whether it was.
  bool checkLive(const Path &path, mlir::Operation *op, llvm::StringRef verb) {
    auto *owner = lookup(path);
    if (!owner || owner->kind == Owner::Live)
      return true;
    if (!reports_)
      return false;
    // Through a pointer that isn't live, the deref that reached the part
    // reported the pointer, which is the mistake: what it pointed at went
    // with it.
    for (size_t i = 0; i < path.steps.size(); ++i)
      if (path.steps[i].kind == Step::Pointee) {
        Path pointer{path.root, {path.steps.begin(), path.steps.begin() + i}};
        if (auto *p = lookup(pointer); p && p->kind != Owner::Live)
          return false;
      }
    reports_->reported(path);
    auto name = "'" + nameOf(path) + "'";
    auto prefix = "Cannot " + verb.str() + " " + name + " — ";
    if (owner->isEmpty()) {
      reports_->error(op->getLoc(),
                      prefix + "no owned value is known to be stored there");
      return false;
    }
    std::string when;
    bool loop = false;
    if (owner->kind == Owner::Moved) {
      when = "it was already " + owner->reason.str() + " previously";
    } else if (owner->movedBy && reports_->onOneCycle(owner->movedBy, op)) {
      loop = true;
      when =
          "it was " + owner->reason.str() + " in an earlier turn of the loop";
    } else {
      when = "it was " + owner->reason.str() + " in some branches";
    }
    auto diag = reports_->error(op->getLoc(), prefix + when);
    // Moved by this very op on an earlier turn: the error shows where.
    if (!loop || owner->movedBy != op)
      diag.attachNote(mlir::Location(owner->movedAt))
          << name << " was " << owner->reason << " here";
    return false;
  }

  /// Moves the whole of what `value` names, for `op`.
  void take(mlir::Value value, mlir::Operation *op, llvm::StringRef why) {
    auto path = pathOf(value);
    if (!path || !lookup(*path))
      return;
    if (checkLive(*path, op, "use linear variable") && reports_)
      // Its parts must all be there; what an owned pointer points at goes
      // with it whatever it holds.
      for (auto &[part, owner] : owners_) {
        if (!path->contains(part) || part == *path ||
            owner.kind == Owner::Live || owner.isEmpty())
          continue;
        if (llvm::any_of(llvm::drop_begin(part.steps, path->steps.size()),
                         [](Step s) { return s.kind == Step::Pointee; }))
          continue;
        auto name = "'" + nameOf(*path) + "'";
        auto partName = "'" + nameOf(part) + "'";
        reports_->error(op->getLoc(), "Cannot move " + name + " — its part " +
                                          partName + " was already " +
                                          owner.reason.str())
                .attachNote(mlir::Location(owner.movedAt))
            << partName << " was " << owner.reason << " here";
        break;
      }
    markMoved(*path, op, why);
  }

  /// Looks at what `value` names, through `op`.
  void look(mlir::Value value, mlir::Operation *op, llvm::StringRef verb) {
    if (auto path = nearestTracked(value))
      checkLive(*path, op, verb);
  }

  /// Why the value `result` is taken, by what uses it.
  static llvm::StringRef takenFor(mlir::Value result) {
    if (result.hasOneUse())
      return reasonFor(*result.getUsers().begin());
    return "moved";
  }

  /// Every owner in `path`'s tree still holding memory, or holding it on
  /// some paths only. An owned pointer stands for what it points at.
  void liveOwners(const Path &path, llvm::SmallVectorImpl<Path> &out) const {
    auto *owner = lookup(path);
    if (!owner || owner->isEmpty())
      return;
    if (owner->kind == Owner::MaybeMoved) {
      out.push_back(path);
      return;
    }
    auto type = typeOf(path);
    auto parts = partsOf(type);
    bool hasParts =
        llvm::any_of(parts, [&](Step s) { return lookup(path.child(s)); });
    if (owner->kind == Owner::Live && (isOwnedPtr(type) || !hasParts)) {
      out.push_back(path);
      return;
    }
    for (auto step : parts)
      liveOwners(path.child(step), out);
  }

  void reportLeak(const Path &path) {
    if (!reports_ || !reports_->firstLeak(path))
      return;
    auto &owner = owners_[path];
    auto name = nameOf(path);
    if (name.empty()) {
      reports_->error(definedAt(path.root),
                      "Temporary of linear type '" +
                          smn::spelling(path.root.getType()) +
                          "' must not be discarded (bind it with 'let' "
                          "first)");
    } else if (owner.kind == Owner::MaybeMoved) {
      reports_->error(mlir::Location(owner.definedAt),
                      "Linear variable '" + name +
                          "' is consumed in some branches but not others")
              .attachNote(mlir::Location(owner.movedAt))
          << "'" << name << "' was " << owner.reason << " here";
    } else {
      reports_->error(mlir::Location(owner.definedAt),
                      "Linear variable '" + name +
                          "' must be consumed before scope exit (use free() "
                          "to deallocate)");
    }
  }

  /// What is still owned where the function returns.
  void checkLeaks() {
    llvm::SmallVector<Path> roots;
    for (auto &[path, owner] : owners_)
      if (path.steps.empty())
        roots.push_back(path);
    for (auto &root : roots) {
      llvm::SmallVector<Path> live;
      liveOwners(root, live);
      // A value no binding names is reported once, as a whole.
      if (!live.empty() && !bindingName(root.root)) {
        reportLeak(root);
        continue;
      }
      for (auto &path : live)
        reportLeak(path);
    }
  }

  /// A call can't lend what it also takes: the callee could free it while
  /// still reading the view.
  void checkLendAndMove(mlir::Operation *call) {
    if (!reports_)
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
        Path shown = *place;
        if (!shown.steps.empty() && shown.steps.back().kind == Step::Pointee)
          shown.steps.pop_back();
        reports_->error(call->getLoc(), "Cannot lend '" + nameOf(shown) +
                                            "' and move it in the same call");
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
    take(let.getValue(), op, "moved");
    handOn(before, owners_, pathOf(let.getValue()), Path{let.getResult(), {}},
           loc);
    return;
  }
  if (auto store = mlir::dyn_cast<smn::StoreOp>(op)) {
    auto value = store.getValue();
    if (!smn::ownsMemory(value.getType()))
      return;
    Owners before = owners_;
    take(value, op, "moved");
    auto target = pathOf(store.getPlace());
    if (!target || !lookup(*target))
      return;
    // A place takes a value only once what it held is gone.
    llvm::SmallVector<Path> live;
    liveOwners(*target, live);
    if (llvm::any_of(live, [&](const Path &p) {
          return lookup(p)->kind == Owner::Live;
        })) {
      if (reports_)
        reports_->error(loc, "Reassigning linear variable '" + nameOf(*target) +
                                 "' without consuming its previous value");
      return;
    }
    for (auto &[path, owner] : Owners(owners_))
      if (target->contains(path))
        owners_.erase(path);
    handOn(before, owners_, pathOf(value), *target, loc);
    return;
  }
  if (auto move = mlir::dyn_cast<smn::MoveOp>(op)) {
    auto place = pathOf(move.getPlace());
    Path result{move.getResult(), {}};
    if (!place)
      return;
    if (onlyLookedAt(move.getResult())) {
      auto *user = *move.getResult().getUsers().begin();
      checkLive(*place, op,
                mlir::isa<smn::DerefOp>(user)     ? "dereference"
                : mlir::isa<smn::PtrElemOp>(user) ? "index"
                                                  : "use");
      return;
    }
    if (!lookup(*place)) {
      fresh(owners_, result, loc);
      return;
    }
    Owners before = owners_;
    checkLive(*place, op, "use linear variable");
    handOn(before, owners_, place, result, std::nullopt);
    markMoved(*place, op, takenFor(move.getResult()));
    return;
  }
  if (auto free = mlir::dyn_cast<smn::FreeOp>(op)) {
    auto path = pathOf(free.getPtr());
    if (path && lookup(*path) && lookup(*path)->kind == Owner::Live)
      // Freeing an owner frees only its own memory: what it points at must
      // hold nothing by now.
      for (auto step : partsOf(typeOf(*path))) {
        llvm::SmallVector<Path> live;
        liveOwners(path->child(step), live);
        for (auto &part : live)
          reportLeak(part);
      }
    take(free.getPtr(), op, "freed");
    return;
  }
  if (auto ret = mlir::dyn_cast<mlir::func::ReturnOp>(op)) {
    for (auto operand : ret.getOperands())
      take(operand, op, "returned");
    checkLeaks();
    return;
  }
  if (auto tag = mlir::dyn_cast<smn::TagOp>(op)) {
    // A case takes the value apart: its payloads go to the arms' bindings,
    // and the value itself is gone.
    auto path = pathOf(tag.getValue());
    if (!path || !lookup(*path))
      return;
    // One already gone is reported here; the arms find its payloads as if
    // it weren't, so they report nothing more.
    if (!checkLive(*path, op, "use linear variable"))
      fresh(owners_, *path, loc);
    auto &owner = owners_[*path];
    owner.kind = Owner::Moved;
    owner.movedAt = loc;
    owner.movedBy = op;
    owner.reason = "matched in case";
    return;
  }
  if (auto deref = mlir::dyn_cast<smn::DerefOp>(op)) {
    if (isOwnedPtr(deref.getPtr().getType()))
      look(deref.getPtr(), op, "dereference");
    return;
  }
  if (auto ptrElem = mlir::dyn_cast<smn::PtrElemOp>(op)) {
    if (isOwnedPtr(ptrElem.getPtr().getType()))
      look(ptrElem.getPtr(), op, "index");
    return;
  }
  if (mlir::isa<smn::LendOp, smn::AddrOp>(op)) {
    // A view of what an owned pointer points at is a view through the
    // pointer: it may see it hold nothing, as the pointer's type allows.
    auto verb = mlir::isa<smn::AddrOp>(op) ? "take the address of" : "use";
    auto place = pathOf(op->getOperand(0));
    if (place && !place->steps.empty() &&
        place->steps.back().kind == Step::Pointee) {
      place->steps.pop_back();
      if (lookup(*place))
        checkLive(*place, op, verb);
      return;
    }
    look(op->getOperand(0), op, verb);
    return;
  }
  if (mlir::isa<smn::CopyOp, smn::AddressOp>(op)) {
    look(op->getOperand(0), op, "use");
    return;
  }
  if (mlir::isa<smn::ExtractOp, smn::PayloadOp, smn::ElementOp>(op)) {
    // A part that owns memory is taken where it is used; reading one that
    // owns nothing reads what it is part of.
    if (!smn::ownsMemory(op->getResult(0).getType()))
      look(op->getResult(0), op, "use");
    return;
  }
  if (auto local = mlir::dyn_cast<smn::LocalOp>(op)) {
    // A slot holds nothing until something is stored there.
    if (smn::ownsMemory(local.getPlace().getType().getValueType()))
      fresh(owners_, Path{local.getPlace(), {}}, loc, /*empty=*/true);
    return;
  }
  if (mlir::isa<smn::FieldOp, smn::ElemOp>(op))
    return;

  // Anything else takes what it is given and makes new values: a struct,
  // tuple, array or variant owns what it is built from.
  if (mlir::isa<smn::CallOp, smn::CallIndirectOp>(op))
    checkLendAndMove(op);
  Owners before = owners_;
  for (auto operand : op->getOperands())
    take(operand, op, reasonFor(op));
  bool builds = mlir::isa<smn::AggregateOp, smn::VariantOp>(op);
  for (auto result : op->getResults()) {
    if (!smn::ownsMemory(result.getType()))
      continue;
    Path path{result, {}};
    if (!builds) {
      fresh(owners_, path, loc);
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
    take(operand, op, "moved");
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

// --- The analysis ---------------------------------------------------------

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

  // A function's arguments are live where it starts; what an owned one
  // points at holds nothing, as far as the function knows.
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

bool checkFunction(mlir::func::FuncOp func) {
  mlir::DataFlowSolver solver(mlir::DataFlowConfig().setInterprocedural(false));
  solver.load<df::DeadCodeAnalysis>();
  solver.load<UnknownConstants>();
  solver.load<OwnershipAnalysis>();
  if (mlir::failed(solver.initializeAndRun(func))) {
    func.emitError("the ownership check could not analyze this function");
    return true;
  }
  Reports reports;
  func.walk([&](mlir::Operation *op) {
    auto *executable = solver.lookupState<df::Executable>(
        solver.getProgramPointBefore(op->getBlock()));
    if (!executable || !executable->isLive())
      return;
    auto *state =
        solver.lookupState<OwnershipState>(solver.getProgramPointBefore(op));
    if (!state)
      return;
    auto owners = state->owners();
    if (auto branch = mlir::dyn_cast<mlir::BranchOpInterface>(op)) {
      // Each value a branch passes is checked once, on its first edge.
      llvm::DenseSet<mlir::Value> checked;
      for (unsigned i = 0; i < branch->getNumSuccessors(); ++i) {
        auto edge = owners;
        bool fresh = llvm::any_of(
            branch.getSuccessorOperands(i).getForwardedOperands(),
            [&](mlir::Value v) { return checked.insert(v).second; });
        Transfer(edge, fresh ? &reports : nullptr).edge(branch, i);
      }
      return;
    }
    Transfer(owners, &reports).apply(op);
  });
  return reports.failed();
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
