#include "dialect/LambdaSets.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Transforms/RegionUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

//! \file LambdaSets.cpp
//! \brief Lambda sets on MIR, one per flow of function values across the
//! module (the paper's sets without its per-call-site polymorphism, as
//! MLton has them), and the rewrite to what closures bind.
//!
//! Every `!sammine.fn` in a value's type, in pre-order through pointees,
//! fields, elements, variants and signatures, has a set variable. Each op
//! that moves a value unifies the variables of where it comes from and
//! where it goes; a `sammine.closure` puts itself in its result's set.

namespace sammine_lang {

namespace {

// --- Shapes ---------------------------------------------------------------

/// The types a type's own is built of, in pre-order.
llvm::SmallVector<mlir::Type> partsOf(mlir::Type type) {
  if (auto ptr = mlir::dyn_cast<smn::PtrType>(type))
    return {ptr.getPointee()};
  if (auto place = mlir::dyn_cast<smn::PlaceType>(type))
    return {place.getValueType()};
  if (auto st = mlir::dyn_cast<smn::StructType>(type))
    return llvm::to_vector(st.getFields());
  if (auto tuple = mlir::dyn_cast<smn::TupleType>(type))
    return llvm::to_vector(tuple.getElements());
  if (auto array = mlir::dyn_cast<smn::ArrayType>(type))
    return {array.getElement()};
  if (auto en = mlir::dyn_cast<smn::EnumType>(type))
    return llvm::to_vector(en.getVariants());
  if (auto fn = mlir::dyn_cast<smn::FnType>(type)) {
    auto parts = llvm::to_vector(fn.getSignature().getInputs());
    parts.append(fn.getSignature().getResults().begin(),
                 fn.getSignature().getResults().end());
    return parts;
  }
  return {};
}

/// How many function types `type` has, itself included.
unsigned fnCount(mlir::Type type) {
  unsigned count = mlir::isa<smn::FnType>(type) ? 1 : 0;
  for (auto part : partsOf(type))
    count += fnCount(part);
  return count;
}

/// Where part `i` of `type` starts among its function types.
unsigned partOffset(mlir::Type type, unsigned i) {
  unsigned offset = mlir::isa<smn::FnType>(type) ? 1 : 0;
  auto parts = partsOf(type);
  for (unsigned j = 0; j < i && j < parts.size(); ++j)
    offset += fnCount(parts[j]);
  return offset;
}

/// Calls `fn(i, part, first)` for each part of `type`, with where its
/// variables start if `type`'s start at `first`.
template <typename Fn> void forEachPart(mlir::Type type, int first, Fn fn) {
  int at = first + (mlir::isa<smn::FnType>(type) ? 1 : 0);
  for (auto [i, part] : llvm::enumerate(partsOf(type))) {
    fn(static_cast<unsigned>(i), part, at);
    at += static_cast<int>(fnCount(part));
  }
}

/// Where a variable starts in `type`, and what it counts there.
struct Span {
  int first = -1;
  unsigned count = 0;
  Span part(mlir::Type type, unsigned i) const {
    auto parts = partsOf(type);
    return {first + static_cast<int>(partOffset(type, i)), fnCount(parts[i])};
  }
};

// --- The analysis ----------------------------------------------------------

class LambdaSets {
public:
  /// `opaque` names functions whose calls and closures are followed no
  /// further than what they are given: what they do with it isn't known.
  explicit LambdaSets(mlir::ModuleOp module,
                      const llvm::StringSet<> *opaque = nullptr)
      : module_(module), opaque_(opaque) {
    for (auto func : module.getOps<mlir::func::FuncOp>())
      if (smn::definesMir(func)) {
        funcs_.push_back(func);
        byName_[func.getName()] = func;
      }
    // Every signature first: a body may call a function defined after it.
    for (auto func : funcs_)
      declare(func);
    for (auto func : funcs_)
      func.walk([&](mlir::Operation *op) { collect(op, func); });
    solve();
  }

  /// Whether the function value whose variable is `var` becomes what its
  /// closure binds.
  bool byValue(int var) { return var >= 0 && !stays_[at(find(var))]; }

  /// The variable of the function type at the start of `value`'s type.
  int varOf(mlir::Value value) { return spanOf(value).first; }

  /// What `type`, `value`'s or a part of it whose variables start at
  /// `first`, becomes.
  mlir::Type rebuild(mlir::Type type, int first);
  mlir::Type rebuild(mlir::Value value) {
    return rebuild(value.getType(), spanOf(value).first);
  }

  /// The closures of `var`'s set, one per description, each to its
  /// variant: two closures calling one function with alike bound values
  /// are one variant.
  llvm::ArrayRef<smn::ClosureOp> closures(int var);
  unsigned variantOf(smn::ClosureOp closure) {
    (void)closures(varOf(closure.getResult()));
    return variants_.lookup(closure);
  }
  /// What a value of `var`'s set is. Sets of one description are one type.
  mlir::Type representation(int var);

  /// What the function values of `type`, `value`'s or a part of it whose
  /// variables start at `first`, may be: by the functions their closures
  /// call and what those bind. Values of alike descriptions are made alike.
  std::string describe(mlir::Type type, int first);
  std::string describe(smn::ClosureOp closure);
  /// What `func`'s signature takes and gives, described.
  std::string describe(mlir::func::FuncOp func);

  llvm::ArrayRef<mlir::func::FuncOp> funcs() const { return funcs_; }
  Span resultSpan(mlir::func::FuncOp func, unsigned i) {
    return results_[func][i];
  }

private:
  mlir::ModuleOp module_;
  const llvm::StringSet<> *opaque_;
  bool opaque(llvm::StringRef name) const {
    return opaque_ && opaque_->contains(name);
  }
  llvm::SmallVector<mlir::func::FuncOp> funcs_;
  /// The functions MIR defines, by name.
  llvm::StringMap<mlir::func::FuncOp> byName_;

  /// A variable as an index into the per-variable vectors.
  static size_t at(int v) { return static_cast<size_t>(v); }

  std::vector<int> parent_;
  /// Per variable: whether it stays a `!sammine.fn`.
  std::vector<bool> stays_;
  /// Per variable at a function type: how many variables its signature
  /// has after it.
  std::vector<unsigned> signature_;
  llvm::DenseMap<mlir::Value, Span> spans_;
  llvm::DenseMap<mlir::Operation *, llvm::SmallVector<Span>> results_;
  /// Per set, by its root: its closures.
  std::map<int, llvm::SmallVector<smn::ClosureOp>> closures_;
  std::map<int, mlir::Type> representations_;
  /// Per set, by its root: its closures, one per description, in order.
  std::map<int, llvm::SmallVector<smn::ClosureOp>> distinct_;
  /// Per closure: its variant, its index in its set's distinct_.
  llvm::DenseMap<mlir::Operation *, unsigned> variants_;
  llvm::DenseMap<mlir::Operation *, std::string> closureDescriptions_;
  /// Per set description: its number, which names its enum and stands for
  /// it in the descriptions of others.
  std::map<std::string, unsigned> setNames_;
  std::map<int, unsigned> setNumbers_;
  unsigned setNumber(int var);

  int find(int v) {
    while (parent_[at(v)] != v)
      v = parent_[at(v)] = parent_[at(parent_[at(v)])];
    return v;
  }
  void join(int a, int b) {
    a = find(a);
    b = find(b);
    if (a == b)
      return;
    parent_[at(b)] = a;
    if (stays_[at(b)])
      stays_[at(a)] = true;
    auto from = closures_.find(b);
    if (from != closures_.end()) {
      auto moved = std::move(from->second);
      closures_.erase(from);
      closures_[a].append(moved.begin(), moved.end());
    }
  }
  void unify(Span a, Span b) {
    for (unsigned k = 0; k < std::min(a.count, b.count); ++k)
      join(a.first + static_cast<int>(k), b.first + static_cast<int>(k));
  }
  /// Makes `span`'s sets stay; true if one didn't already.
  bool stay(Span span) {
    bool changed = false;
    for (unsigned k = 0; k < span.count; ++k) {
      auto root = at(find(span.first + static_cast<int>(k)));
      changed |= !stays_[root];
      stays_[root] = true;
    }
    return changed;
  }

  /// Fresh variables for `type`.
  Span fresh(mlir::Type type) {
    Span span{static_cast<int>(parent_.size()), fnCount(type)};
    addVariables(type);
    return span;
  }
  void addVariables(mlir::Type type) {
    if (mlir::isa<smn::FnType>(type)) {
      parent_.push_back(static_cast<int>(parent_.size()));
      stays_.push_back(false);
      signature_.push_back(fnCount(type) - 1);
    }
    for (auto part : partsOf(type))
      addVariables(part);
  }
  Span spanOf(mlir::Value value) {
    auto it = spans_.find(value);
    if (it != spans_.end())
      return it->second;
    auto span = fresh(value.getType());
    spans_[value] = span;
    return span;
  }
  mlir::func::FuncOp defined(mlir::StringRef name) {
    return byName_.lookup(name);
  }

  void declare(mlir::func::FuncOp func);
  void collect(mlir::Operation *op, mlir::func::FuncOp func);
  void solve();
};

void LambdaSets::declare(mlir::func::FuncOp func) {
  auto &results = results_[func];
  for (auto type : func.getFunctionType().getResults())
    results.push_back(fresh(type));
  for (auto arg : func.getArguments())
    spanOf(arg);
  // Called from another unit, the function is given and gives values of
  // its types as they are: a library's own exported functions.
  if (!func.isPrivate()) {
    for (auto arg : func.getArguments())
      stay(spanOf(arg));
    for (auto span : results)
      stay(span);
  }
}

void LambdaSets::collect(mlir::Operation *op, mlir::func::FuncOp func) {
  auto whole = [&](mlir::Value a, mlir::Value b) {
    unify(spanOf(a), spanOf(b));
  };
  if (auto closure = mlir::dyn_cast<smn::ClosureOp>(op)) {
    auto result = spanOf(closure.getResult());
    if (opaque(closure.getCallee())) {
      closures_[find(result.first)].push_back(closure);
      return;
    }
    auto fnType = closure.getType();
    auto callee = defined(closure.getCallee());
    if (!callee) {
      // C or another module's: its code takes values as they are.
      stay(result);
      for (auto bound : closure.getBound())
        stay(spanOf(bound));
      return;
    }
    closures_[find(result.first)].push_back(closure);
    auto params = callee.getArguments();
    auto bound = closure.getBound();
    for (auto [i, value] : llvm::enumerate(bound))
      unify(spanOf(value), spanOf(params[i]));
    auto inputs = fnType.getSignature().getInputs().size();
    for (unsigned j = 0; j < inputs; ++j)
      unify(result.part(fnType, j), spanOf(params[bound.size() + j]));
    for (unsigned j = 0; j < fnType.getSignature().getNumResults(); ++j)
      unify(result.part(fnType, static_cast<unsigned>(inputs) + j),
            results_[callee][j]);
  } else if (auto call = mlir::dyn_cast<smn::CallIndirectOp>(op)) {
    auto callee = spanOf(call.getCallee());
    auto fnType = call.getCallee().getType();
    auto inputs = fnType.getSignature().getInputs().size();
    for (auto [i, arg] : llvm::enumerate(call.getArgs()))
      unify(spanOf(arg), callee.part(fnType, static_cast<unsigned>(i)));
    for (auto [i, result] : llvm::enumerate(call.getResults()))
      unify(spanOf(result),
            callee.part(fnType, static_cast<unsigned>(inputs + i)));
  } else if (auto direct = mlir::dyn_cast<smn::CallOp>(op)) {
    if (opaque(direct.getCallee()))
      return;
    if (auto callee = defined(direct.getCallee())) {
      for (auto [arg, param] :
           llvm::zip(direct.getArgs(), callee.getArguments()))
        whole(arg, param);
      for (auto [i, result] : llvm::enumerate(direct.getResults()))
        unify(spanOf(result), results_[callee][i]);
    } else {
      for (auto arg : direct.getArgs())
        stay(spanOf(arg));
      for (auto result : direct.getResults())
        stay(spanOf(result));
    }
  } else if (auto ret = mlir::dyn_cast<mlir::func::ReturnOp>(op)) {
    for (auto [i, value] : llvm::enumerate(ret.getOperands()))
      unify(spanOf(value), results_[func][i]);
  } else if (auto branch = mlir::dyn_cast<mlir::BranchOpInterface>(op)) {
    smn::forEachBranchEdge(branch, whole);
  } else if (auto let = mlir::dyn_cast<smn::LetOp>(op)) {
    whole(let.getResult(), let.getValue());
  } else if (mlir::isa<smn::CopyOp, smn::MoveOp>(op)) {
    // A place's variables are its value's.
    whole(op->getResult(0), op->getOperand(0));
  } else if (auto store = mlir::dyn_cast<smn::StoreOp>(op)) {
    whole(store.getValue(), store.getPlace());
  } else if (mlir::isa<smn::DerefOp, smn::AddrOp, smn::LendOp, smn::ThroughOp,
                       smn::PtrElemOp>(op)) {
    // A pointer's, a place's and its pointee's variables are the same.
    whole(op->getResult(0), op->getOperand(0));
  } else if (auto field = mlir::dyn_cast<smn::FieldOp>(op)) {
    auto base = field.getBase().getType().getValueType();
    unify(spanOf(field.getPlace()),
          spanOf(field.getBase())
              .part(base, static_cast<unsigned>(field.getIndex())));
  } else if (mlir::isa<smn::ElemOp, smn::ElementOp>(op)) {
    // An element's variables are the array's, from its start.
    unify(spanOf(op->getResult(0)), spanOf(op->getOperand(0)));
  } else if (auto aggregate = mlir::dyn_cast<smn::AggregateOp>(op)) {
    auto type = aggregate.getType();
    auto span = spanOf(aggregate.getResult());
    bool array = mlir::isa<smn::ArrayType>(type);
    for (auto [i, value] : llvm::enumerate(aggregate.getFields()))
      unify(spanOf(value),
            span.part(type, array ? 0 : static_cast<unsigned>(i)));
  } else if (auto extract = mlir::dyn_cast<smn::ExtractOp>(op)) {
    auto type = extract.getAggregate().getType();
    bool array = mlir::isa<smn::ArrayType>(type);
    unify(
        spanOf(extract.getResult()),
        spanOf(extract.getAggregate())
            .part(type, array ? 0 : static_cast<unsigned>(extract.getIndex())));
  } else if (auto variant = mlir::dyn_cast<smn::VariantOp>(op)) {
    auto type = variant.getType();
    auto index = static_cast<unsigned>(variant.getIndex());
    auto payload = spanOf(variant.getResult()).part(type, index);
    auto tuple = type.getVariants()[index];
    for (auto [i, value] : llvm::enumerate(variant.getPayload()))
      unify(spanOf(value), payload.part(tuple, static_cast<unsigned>(i)));
  } else if (auto payload = mlir::dyn_cast<smn::PayloadOp>(op)) {
    auto type = payload.getValue().getType();
    auto index = static_cast<unsigned>(payload.getVariant());
    auto fields = spanOf(payload.getValue()).part(type, index);
    unify(spanOf(payload.getResult()),
          fields.part(type.getVariants()[index],
                      static_cast<unsigned>(payload.getField())));
  } else if (mlir::isa<smn::DropOp, smn::TagOp, smn::AddressOp, smn::AllocOp,
                       smn::LocalOp>(op)) {
    // Moves no function value: an alloc's or a local's is fresh.
  } else {
    // An op this doesn't follow: what it touches stays as it is.
    for (auto value : op->getOperands())
      stay(spanOf(value));
    for (auto value : op->getResults())
      stay(spanOf(value));
  }
}

void LambdaSets::solve() {
  // A set that stays keeps the sets in its signature and those its
  // closures bind as they are: its code is a stage-1 wrapper, passed and
  // giving values of its types as they are, and a stage-1 closure carries
  // what it binds in words sized for those types.
  auto spread = [&]() {
    for (bool changed = true; changed;) {
      changed = false;
      for (int v = 0; v < static_cast<int>(parent_.size()); ++v)
        if (stays_[at(find(v))])
          changed |= stay({v + 1, signature_[at(v)]});
      for (auto &[root, closures] : closures_) {
        if (!stays_[at(find(root))])
          continue;
        for (auto closure : closures)
          for (auto bound : closure.getBound())
            changed |= stay(spanOf(bound));
      }
    }
  };
  spread();

  // A set that holds itself through what its closures bind would have no
  // end of size: it stays (the paper's μ sets).
  for (bool changed = true; changed;) {
    changed = false;
    std::map<int, llvm::SmallVector<int>> holds;
    for (auto &[root, closures] : closures_) {
      if (stays_[at(find(root))])
        continue;
      for (auto closure : closures)
        for (auto bound : closure.getBound()) {
          // What a value binds: the sets of its type outside signatures.
          std::function<void(mlir::Type, int)> walk = [&](mlir::Type t,
                                                          int first) {
            if (mlir::isa<smn::FnType>(t)) {
              holds[find(root)].push_back(find(first));
              return;
            }
            forEachPart(t, first, [&](unsigned, mlir::Type part, int start) {
              walk(part, start);
            });
          };
          walk(bound.getType(), spanOf(bound).first);
        }
    }
    // Each set that reaches itself.
    for (auto &[root, _] : holds) {
      llvm::SmallVector<int> work(holds[root].begin(), holds[root].end());
      llvm::DenseSet<int> seen;
      while (!work.empty()) {
        int next = find(work.pop_back_val());
        if (next == find(root)) {
          stays_[at(find(root))] = true;
          changed = true;
          break;
        }
        if (!seen.insert(next).second)
          continue;
        auto it = holds.find(next);
        if (it != holds.end())
          work.append(it->second.begin(), it->second.end());
      }
    }
    if (changed)
      spread();
  }
}

llvm::ArrayRef<smn::ClosureOp> LambdaSets::closures(int var) {
  int root = find(var);
  if (auto it = distinct_.find(root); it != distinct_.end())
    return it->second;
  std::map<std::string, llvm::SmallVector<smn::ClosureOp>> byDescription;
  for (auto closure : closures_[root])
    byDescription[describe(closure)].push_back(closure);
  auto &out = distinct_[root];
  for (auto &[_, alike] : byDescription) {
    for (auto closure : alike)
      variants_[closure] = static_cast<unsigned>(out.size());
    out.push_back(alike.front());
  }
  return out;
}

std::string LambdaSets::describe(smn::ClosureOp closure) {
  if (auto it = closureDescriptions_.find(closure);
      it != closureDescriptions_.end())
    return it->second;
  std::string out = closure.getCallee().str() + "(";
  for (auto [i, bound] : llvm::enumerate(closure.getBound()))
    out += (i ? ", " : "") + describe(bound.getType(), spanOf(bound).first);
  out += ")";
  closureDescriptions_[closure] = out;
  return out;
}

unsigned LambdaSets::setNumber(int var) {
  int root = find(var);
  if (auto it = setNumbers_.find(root); it != setNumbers_.end())
    return it->second;
  // A set's closures don't reach it again: one that would stays. What they
  // bind is described by the numbers of its sets, so a description is as
  // long as its closures, however deep they nest (the paper's 6.1).
  std::string description = "{";
  for (auto [i, closure] : llvm::enumerate(closures(root)))
    description += (i ? ", " : "") + describe(closure);
  description += "}";
  auto number =
      setNames_
          .try_emplace(description, static_cast<unsigned>(setNames_.size()))
          .first->second;
  setNumbers_[root] = number;
  return number;
}

std::string LambdaSets::describe(mlir::Type type, int first) {
  std::string out;
  llvm::raw_string_ostream os(out);
  if (fnCount(type) == 0) {
    os << type;
    return out;
  }
  if (mlir::isa<smn::FnType>(type)) {
    if (!byValue(first)) {
      os << "stays " << type;
      return out;
    }
    os << "set" << setNumber(first);
    return out;
  }
  os << type << " with (";
  forEachPart(type, first, [&](unsigned i, mlir::Type part, int start) {
    os << (i ? ", " : "") << describe(part, start);
  });
  os << ")";
  return out;
}

std::string LambdaSets::describe(mlir::func::FuncOp func) {
  std::string out = "(";
  for (auto [i, arg] : llvm::enumerate(func.getArguments()))
    out += (i ? ", " : "") + describe(arg.getType(), spanOf(arg).first);
  out += ") -> (";
  for (auto [i, type] : llvm::enumerate(func.getFunctionType().getResults()))
    out += (i ? ", " : "") +
           describe(type, results_[func][static_cast<unsigned>(i)].first);
  return out + ")";
}

mlir::Type LambdaSets::representation(int var) {
  int root = find(var);
  if (auto it = representations_.find(root); it != representations_.end())
    return it->second;
  auto *ctx = module_.getContext();
  llvm::SmallVector<mlir::Type> variants;
  for (auto closure : closures(root)) {
    llvm::SmallVector<mlir::Type> bound;
    for (auto value : closure.getBound())
      bound.push_back(rebuild(value));
    variants.push_back(smn::TupleType::get(ctx, bound));
  }
  mlir::Type type;
  if (variants.size() <= 1) {
    type = variants.empty() ? smn::TupleType::get(ctx, {}) : variants.front();
  } else {
    llvm::SmallVector<mlir::StringAttr> names;
    for (unsigned i = 0; i < variants.size(); ++i)
      names.push_back(
          mlir::StringAttr::get(ctx, "closure" + std::to_string(i)));
    // Named by what it holds: sets alike are one type, as a function
    // specialized for one passes it to another.
    type = smn::makeEnum(ctx, "lambdas" + std::to_string(setNumber(root)),
                         names, variants);
  }
  representations_[root] = type;
  return type;
}

mlir::Type LambdaSets::rebuild(mlir::Type type, int first) {
  if (fnCount(type) == 0)
    return type;
  auto *ctx = type.getContext();
  if (mlir::isa<smn::FnType>(type))
    return byValue(first) ? representation(first) : type;
  auto parts = partsOf(type);
  llvm::SmallVector<mlir::Type> rebuilt;
  forEachPart(type, first, [&](unsigned, mlir::Type part, int start) {
    rebuilt.push_back(rebuild(part, start));
  });
  if (llvm::equal(parts, rebuilt))
    return type;
  // A struct or enum holding other function values keeps its name: it is
  // another type by its fields, as MIR's types are told apart.
  if (auto ptr = mlir::dyn_cast<smn::PtrType>(type))
    return smn::PtrType::get(ctx, rebuilt[0], ptr.getOwnership());
  if (mlir::isa<smn::PlaceType>(type))
    return smn::PlaceType::get(ctx, rebuilt[0]);
  if (auto st = mlir::dyn_cast<smn::StructType>(type))
    return smn::StructType::get(ctx, st.getName(), st.getFieldNames(), rebuilt);
  if (mlir::isa<smn::TupleType>(type))
    return smn::TupleType::get(ctx, rebuilt);
  if (auto array = mlir::dyn_cast<smn::ArrayType>(type))
    return smn::ArrayType::get(ctx, array.getSize(), rebuilt[0]);
  if (auto en = mlir::dyn_cast<smn::EnumType>(type))
    return smn::makeEnum(ctx, en.getName(), en.getVariantNames(), rebuilt);
  return type;
}

// --- The rewrite -----------------------------------------------------------

/// A closure of a set that becomes what it binds: the tuple of what it
/// binds, or the enum variant of its closure with it.
/// `variant` is its closure's, read before the types changed: the op's
/// result isn't a `!sammine.fn` any more, so it is reached untyped.
void lowerClosure(smn::ClosureOp closure, unsigned variant) {
  mlir::OpBuilder builder(closure);
  auto result = closure->getResult(0);
  auto type = result.getType();
  mlir::Operation *value;
  if (auto en = mlir::dyn_cast<smn::EnumType>(type)) {
    auto fields = smn::variantFields(en, variant);
    value = smn::VariantOp::create(
        builder, closure.getLoc(), en, static_cast<uint64_t>(variant),
        smn::payloadOffsets(fields), closure.getBound());
  } else {
    value = smn::AggregateOp::create(builder, closure.getLoc(), type,
                                     closure.getBound());
  }
  value->setAttr(smn::kCapturesAttr, builder.getUnitAttr());
  result.replaceAllUsesWith(value->getResult(0));
  closure.erase();
}

/// A call through a value of a set that became what it binds: a direct
/// call of its closure's function, given what it binds then the arguments,
/// or, with several closures, a switch on which one it is.
/// `closures` are its set's, read before the types changed.
void lowerCall(smn::CallIndirectOp call,
               llvm::ArrayRef<smn::ClosureOp> closures) {
  auto loc = call.getLoc();
  auto value = call->getOperand(0);
  auto args = llvm::to_vector(call->getOperands().drop_front());
  auto resultTypes = llvm::to_vector(call.getResultTypes());

  auto *block = call->getBlock();
  auto *after = block->splitBlock(call->getIterator());
  llvm::SmallVector<mlir::Location> locs(resultTypes.size(), loc);
  auto merged = after->addArguments(resultTypes, locs);
  for (auto [result, arg] : llvm::zip(call.getResults(), merged))
    result.replaceAllUsesWith(arg);

  mlir::OpBuilder builder(block, block->end());
  // In `at`, the call of `closure`'s function given `bound`, then on.
  auto callIn = [&](mlir::Block *at, smn::ClosureOp closure,
                    llvm::SmallVector<mlir::Value> bound) {
    mlir::OpBuilder b(at, at->end());
    bound.append(args.begin(), args.end());
    auto direct =
        smn::CallOp::create(b, loc, resultTypes, closure.getCallee(), bound);
    mlir::cf::BranchOp::create(b, loc, after, direct.getResults());
  };
  auto boundTypes = [](smn::ClosureOp closure) {
    return llvm::to_vector(closure.getBound().getTypes());
  };

  if (closures.empty()) {
    // No closure reaches it: the call is never made.
    smn::UnreachableOp::create(builder, loc);
  } else if (auto en = mlir::dyn_cast<smn::EnumType>(value.getType())) {
    auto tag = smn::TagOp::create(builder, loc, builder.getI32Type(), value);
    llvm::SmallVector<llvm::APInt> tags;
    llvm::SmallVector<mlir::Block *> dests;
    auto *region = block->getParent();
    for (auto [i, closure] : llvm::enumerate(closures)) {
      auto *dest = new mlir::Block();
      region->getBlocks().insert(after->getIterator(), dest);
      mlir::OpBuilder b(dest, dest->end());
      auto fields = smn::variantFields(en, i);
      auto offsets = smn::payloadOffsets(fields);
      llvm::SmallVector<mlir::Value> bound;
      for (auto [j, field] : llvm::enumerate(fields))
        bound.push_back(smn::PayloadOp::create(
            b, loc, field, value, static_cast<uint64_t>(i),
            static_cast<uint64_t>(j), static_cast<uint64_t>(offsets[j])));
      callIn(dest, closure, bound);
      tags.emplace_back(32, i);
      dests.push_back(dest);
    }
    auto *otherwise = new mlir::Block();
    region->getBlocks().insert(after->getIterator(), otherwise);
    mlir::OpBuilder ob(otherwise, otherwise->end());
    smn::UnreachableOp::create(ob, loc);
    auto caseValues = mlir::DenseIntElementsAttr::get(
        mlir::VectorType::get({static_cast<int64_t>(tags.size())},
                              builder.getI32Type()),
        tags);
    llvm::SmallVector<mlir::ValueRange> noOperands(dests.size());
    mlir::cf::SwitchOp::create(builder, loc, tag, mlir::ValueRange{},
                               noOperands, caseValues, otherwise, dests);
  } else {
    auto closure = closures.front();
    llvm::SmallVector<mlir::Value> bound;
    for (auto [j, type] : llvm::enumerate(boundTypes(closure)))
      bound.push_back(smn::ExtractOp::create(builder, loc, type, value,
                                             static_cast<uint64_t>(j)));
    callIn(block, closure, bound);
  }
  call.erase();
}

/// Each value's type, its function's signature, and the byte offsets the
/// variants and payloads of enums whose payload changed carry.
void retype(LambdaSets &sets, mlir::func::FuncOp func) {
  func.walk([&](mlir::Block *block) {
    for (auto arg : block->getArguments())
      arg.setType(sets.rebuild(arg));
  });
  func.walk([&](mlir::Operation *op) {
    for (auto result : op->getResults())
      result.setType(sets.rebuild(result));
  });
  llvm::SmallVector<mlir::Type> results;
  for (auto [i, type] : llvm::enumerate(func.getFunctionType().getResults()))
    results.push_back(sets.rebuild(
        type, sets.resultSpan(func, static_cast<unsigned>(i)).first));
  func.setFunctionType(mlir::FunctionType::get(
      func.getContext(), func.front().getArgumentTypes(), results));
  func.walk([&](mlir::Operation *op) {
    if (auto variant = mlir::dyn_cast<smn::VariantOp>(op)) {
      variant.setOffsets(smn::payloadOffsets(
          smn::variantFields(variant.getType(), variant.getIndex())));
    } else if (auto payload = mlir::dyn_cast<smn::PayloadOp>(op)) {
      auto fields = smn::variantFields(payload.getValue().getType(),
                                       payload.getVariant());
      payload.setOffset(static_cast<uint64_t>(
          smn::payloadOffsets(fields)[payload.getField()]));
    }
  });
}

struct LowerLambdaSetsPass
    : public mlir::PassWrapper<LowerLambdaSetsPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerLambdaSetsPass)

  llvm::StringRef getArgument() const override {
    return "sammine-lower-lambda-sets";
  }
  llvm::StringRef getDescription() const override {
    return "Make each function value of a known lambda set what its closure "
           "binds, and each call through one a direct call";
  }
  void runOnOperation() override {
    if (mlir::failed(lowerLambdaSets(getOperation())))
      signalPassFailure();
  }
};

} // namespace

bool holdsFunctionValue(mlir::Type type) { return fnCount(type) > 0; }

std::map<mlir::Operation *, std::string>
describeSignatures(mlir::ModuleOp module, const llvm::StringSet<> &opaque) {
  LambdaSets sets(module, &opaque);
  std::map<mlir::Operation *, std::string> out;
  for (auto func : sets.funcs())
    out[func] = sets.describe(func);
  return out;
}

void markLambdaSets(mlir::ModuleOp module) {
  LambdaSets sets(module);
  for (auto func : sets.funcs())
    func.walk([&](smn::ClosureOp closure) {
      if (sets.byValue(sets.varOf(closure.getResult())))
        closure->setAttr(smn::kByValueAttr,
                         mlir::UnitAttr::get(module.getContext()));
    });
}

mlir::LogicalResult lowerLambdaSets(mlir::ModuleOp module) {
  LambdaSets sets(module);
  // What to rewrite, and how, is read before any type changes: sets are
  // read off the types as they were, and every representation is made then.
  llvm::SmallVector<std::pair<smn::ClosureOp, unsigned>> closures;
  llvm::SmallVector<
      std::pair<smn::CallIndirectOp, llvm::SmallVector<smn::ClosureOp>>>
      calls;
  for (auto func : sets.funcs())
    func.walk([&](mlir::Operation *op) {
      if (auto closure = mlir::dyn_cast<smn::ClosureOp>(op)) {
        int var = sets.varOf(closure.getResult());
        if (sets.byValue(var)) {
          (void)sets.representation(var);
          closures.push_back({closure, sets.variantOf(closure)});
        }
      } else if (auto call = mlir::dyn_cast<smn::CallIndirectOp>(op)) {
        int var = sets.varOf(call.getCallee());
        if (sets.byValue(var)) {
          (void)sets.representation(var);
          calls.push_back({call, llvm::to_vector(sets.closures(var))});
        }
      }
    });
  for (auto func : sets.funcs())
    retype(sets, func);
  for (auto &[call, reaching] : calls)
    lowerCall(call, reaching);
  for (auto [closure, variant] : closures)
    lowerClosure(closure, variant);
  // A call no closure reaches is unreachable now, and so is what follows
  // it; lowering to LLVM doesn't convert blocks nothing reaches.
  mlir::IRRewriter rewriter(module.getContext());
  for (auto func : sets.funcs())
    (void)mlir::eraseUnreachableBlocks(rewriter, func->getRegions());
  return mlir::success();
}

std::unique_ptr<mlir::Pass> createLowerLambdaSetsPass() {
  return std::make_unique<LowerLambdaSetsPass>();
}

void registerLowerLambdaSetsPass() {
  mlir::PassRegistration<LowerLambdaSetsPass>();
}

} // namespace sammine_lang
