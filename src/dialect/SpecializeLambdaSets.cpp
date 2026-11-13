#include "dialect/LambdaSets.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/SymbolTable.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

//! \file SpecializeLambdaSets.cpp
//! \brief The paper's specialization step: a higher-order function copied
//! per lambda set it is used with, as generics are per type, so a call
//! through a function value in each copy is one direct call, not a switch.
//!
//! Functions are taken callers first. Each place that calls a private
//! function taking or giving function values, or makes a closure of it,
//! gets its own copy; when the function's turn comes, all its callers are
//! done, and its copies whose signatures have alike sets
//! (describeSignatures) are merged before their own places are copied, so
//! copies follow the sets a function is used with, not every path to it.
//! The functions not yet taken are opaque to that description: their
//! places, all naming the one function yet, would mix the sets of each.
//! Once all are taken, copies are merged again, with every call followed,
//! until none are alike.
//!
//! Functions calling each other (an SCC of several) are left whole, sharing
//! their sets, and a function calling itself does so in its own copy.

namespace sammine_lang {

namespace {

using Func = mlir::func::FuncOp;

/// The most ops copies may add, so a long chain of higher-order calls
/// doesn't grow the module without end: past it, places share a function,
/// as without this step.
constexpr int64_t kCopyBudget = 100000;

/// The function a call or a closure names, or null for any other op.
mlir::FlatSymbolRefAttr calleeOf(mlir::Operation *op) {
  if (auto call = mlir::dyn_cast<smn::CallOp>(op))
    return call.getCalleeAttr();
  if (auto closure = mlir::dyn_cast<smn::ClosureOp>(op))
    return closure.getCalleeAttr();
  return {};
}

/// Per function name: the calls and closures naming it, so that merging
/// and erasing copies don't each walk the module.
class Sites {
public:
  void addIn(mlir::Operation *root) {
    root->walk([&](mlir::Operation *op) {
      if (auto callee = calleeOf(op))
        by_[callee.getValue()].insert(op);
    });
  }
  void removeIn(mlir::Operation *root) {
    root->walk([&](mlir::Operation *op) {
      if (auto callee = calleeOf(op))
        by_[callee.getValue()].remove(op);
    });
  }
  /// Makes `site` name `to`.
  void retarget(mlir::Operation *site, mlir::StringAttr to) {
    by_[calleeOf(site).getValue()].remove(site);
    auto name = mlir::FlatSymbolRefAttr::get(to);
    if (auto call = mlir::dyn_cast<smn::CallOp>(site))
      call.setCalleeAttr(name);
    else
      mlir::cast<smn::ClosureOp>(site).setCalleeAttr(name);
    by_[to.getValue()].insert(site);
  }
  /// Makes everything naming `from` name `to`.
  void redirect(Func from, Func to) {
    auto it = by_.find(from.getName());
    if (it == by_.end())
      return;
    auto moved = it->second.takeVector();
    for (auto *site : moved)
      retarget(site, to.getNameAttr());
  }
  /// Whether anything outside `func` names it.
  bool usedOutside(Func func) {
    auto it = by_.find(func.getName());
    return it != by_.end() &&
           llvm::any_of(it->second, [&](mlir::Operation *site) {
             return !func->isAncestor(site);
           });
  }

private:
  llvm::StringMap<llvm::SetVector<mlir::Operation *>> by_;
};

class Specializer {
public:
  explicit Specializer(mlir::ModuleOp module)
      : module_(module), symbols_(module) {
    for (auto func : module.getOps<Func>())
      if (defined(func))
        copies_[func].push_back(func);
  }

  void run() {
    findNamedOtherwise();
    findSccs();
    sites_.addIn(module_);
    copy();
    for (bool merged = true; merged;) {
      eraseUnused();
      merged = false;
      auto described = describeSignatures(module_);
      for (auto &[op, _] : copies_)
        merged |= mergeCopies(op, described);
    }
  }

private:
  mlir::ModuleOp module_;
  mlir::SymbolTable symbols_;
  /// Per function MIR defines: it, then its copies.
  llvm::MapVector<mlir::Operation *, llvm::SmallVector<Func>> copies_;
  /// Per copy: the function it copies.
  llvm::DenseMap<mlir::Operation *, Func> origins_;
  /// The functions that were copied.
  llvm::DenseSet<mlir::Operation *> copiedFrom_;
  /// Per function: its SCC of the call graph, an index into sccs_, which
  /// has callees before callers.
  llvm::DenseMap<mlir::Operation *, unsigned> scc_;
  std::vector<llvm::SmallVector<Func>> sccs_;
  /// Functions something other than a call or a closure names: copies
  /// wouldn't be found from there.
  llvm::DenseSet<mlir::StringAttr> namedOtherwise_;
  Sites sites_;
  int64_t budget_ = kCopyBudget;
  unsigned copied_ = 0;

  static bool defined(Func func) {
    return func && !func.isDeclaration() && func->hasAttr(smn::kMirAttr);
  }
  Func lookup(mlir::FlatSymbolRefAttr name) {
    auto func = symbols_.lookup<Func>(name.getValue());
    return defined(func) ? func : Func();
  }
  Func originOf(Func func) {
    auto it = origins_.find(func);
    return it == origins_.end() ? func : it->second;
  }

  /// The calls and closures in `func` naming a function MIR defines.
  llvm::SmallVector<mlir::Operation *> sitesIn(Func func) {
    llvm::SmallVector<mlir::Operation *> sites;
    func.walk([&](mlir::Operation *op) {
      if (auto callee = calleeOf(op); callee && lookup(callee))
        sites.push_back(op);
    });
    return sites;
  }

  /// Whether a copy of `func` per place could have other sets: it takes or
  /// gives function values, only this module's calls and closures name it,
  /// and it calls no function that calls it back.
  bool specializable(Func func) {
    // A Drop instance is also found by its name.
    if (!func.isPrivate() || func.getName().starts_with("Drop<") ||
        namedOtherwise_.count(func.getNameAttr()))
      return false;
    if (sccs_[scc_.lookup(func)].size() != 1)
      return false;
    auto type = func.getFunctionType();
    return llvm::any_of(type.getInputs(), holdsFunctionValue) ||
           llvm::any_of(type.getResults(), holdsFunctionValue);
  }

  void findNamedOtherwise();
  void findSccs();
  Func copyOf(Func func);
  void copy();
  bool mergeCopies(mlir::Operation *origin,
                   std::map<mlir::Operation *, std::string> &described);
  void eraseUnused();
};

void Specializer::findNamedOtherwise() {
  auto uses = mlir::SymbolTable::getSymbolUses(&module_.getBodyRegion());
  if (!uses)
    return;
  for (auto &use : *uses)
    if (!calleeOf(use.getUser()))
      namedOtherwise_.insert(use.getSymbolRef().getRootReference());
}

void Specializer::findSccs() {
  // Tarjan's: each SCC is complete when its first function is left, after
  // every SCC it reaches.
  llvm::DenseMap<mlir::Operation *, unsigned> index, low;
  llvm::SmallVector<Func> stack;
  llvm::DenseSet<mlir::Operation *> onStack;
  std::function<void(Func)> visit = [&](Func func) {
    auto *op = func.getOperation();
    index[op] = low[op] = static_cast<unsigned>(index.size());
    stack.push_back(func);
    onStack.insert(op);
    for (auto *site : sitesIn(func)) {
      auto *callee = lookup(calleeOf(site)).getOperation();
      if (!index.count(callee)) {
        visit(mlir::cast<Func>(callee));
        low[op] = std::min(low[op], low[callee]);
      } else if (onStack.count(callee)) {
        low[op] = std::min(low[op], index[callee]);
      }
    }
    if (low[op] != index[op])
      return;
    llvm::SmallVector<Func> scc;
    Func member;
    do {
      member = stack.pop_back_val();
      onStack.erase(member);
      scc_[member] = static_cast<unsigned>(sccs_.size());
      scc.push_back(member);
    } while (member != func);
    sccs_.push_back(std::move(scc));
  };
  for (auto &[op, _] : copies_)
    if (!index.count(op))
      visit(mlir::cast<Func>(op));
}

Func Specializer::copyOf(Func func) {
  auto copy = func.clone();
  std::string name;
  do
    name = (func.getName() + ".spec" + llvm::Twine(++copied_)).str();
  while (symbols_.lookup(name));
  copy.setName(name);
  symbols_.insert(copy, std::next(mlir::Block::iterator(func)));
  sites_.addIn(copy);
  // Calling itself, it calls itself: the copy.
  for (auto *site : sitesIn(copy))
    if (calleeOf(site).getValue() == func.getName())
      sites_.retarget(site, copy.getNameAttr());
  func.walk([&](mlir::Operation *) { --budget_; });
  origins_[copy] = func;
  copiedFrom_.insert(func);
  copies_[func].push_back(copy);
  return copy;
}

void Specializer::copy() {
  // A function keeps its first place; each other place gets a copy.
  llvm::DenseSet<mlir::Operation *> placed;
  // The functions to be copied whose turn hasn't come.
  llvm::StringSet<> waiting;
  for (auto &[op, _] : copies_)
    if (specializable(mlir::cast<Func>(op)))
      waiting.insert(mlir::cast<Func>(op).getName());

  for (auto &scc : llvm::reverse(sccs_)) {
    for (auto func : scc)
      waiting.erase(func.getName());
    for (auto func : scc) {
      if (copies_[func].size() > 1) {
        auto described = describeSignatures(module_, waiting);
        mergeCopies(func, described);
      }
      auto all = copies_[func];
      for (auto from : all)
        for (auto *site : sitesIn(from)) {
          auto callee = originOf(lookup(calleeOf(site)));
          if (scc_.lookup(callee) == scc_.lookup(func) ||
              !specializable(callee) || placed.insert(callee).second ||
              budget_ <= 0)
            continue;
          sites_.retarget(site, copyOf(callee).getNameAttr());
        }
    }
  }
}

bool Specializer::mergeCopies(
    mlir::Operation *origin,
    std::map<mlir::Operation *, std::string> &described) {
  auto &all = copies_[origin];
  if (all.size() < 2)
    return false;
  bool merged = false;
  std::map<std::string, Func> kept;
  llvm::SmallVector<Func> alive;
  for (auto func : all) {
    auto [it, fresh] = kept.try_emplace(described[func], func);
    if (fresh) {
      alive.push_back(func);
      continue;
    }
    sites_.removeIn(func);
    sites_.redirect(func, it->second);
    symbols_.erase(func);
    merged = true;
  }
  all = std::move(alive);
  return merged;
}

void Specializer::eraseUnused() {
  for (bool erased = true; erased;) {
    erased = false;
    for (auto &[op, all] : copies_) {
      // Only a function that was copied, and its copies: others are as the
      // program had them.
      if (!copiedFrom_.count(op))
        continue;
      llvm::SmallVector<Func> alive;
      for (auto func : all) {
        if (func.isPrivate() && !sites_.usedOutside(func)) {
          sites_.removeIn(func);
          symbols_.erase(func);
          erased = true;
        } else {
          alive.push_back(func);
        }
      }
      all = std::move(alive);
    }
  }
}

struct SpecializeLambdaSetsPass
    : public mlir::PassWrapper<SpecializeLambdaSetsPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SpecializeLambdaSetsPass)

  llvm::StringRef getArgument() const override {
    return "sammine-specialize-lambda-sets";
  }
  llvm::StringRef getDescription() const override {
    return "Copy each higher-order function per lambda set it is used with";
  }
  void runOnOperation() override {
    if (mlir::failed(specializeLambdaSets(getOperation())))
      signalPassFailure();
  }
};

} // namespace

mlir::LogicalResult specializeLambdaSets(mlir::ModuleOp module) {
  Specializer(module).run();
  return mlir::success();
}

std::unique_ptr<mlir::Pass> createSpecializeLambdaSetsPass() {
  return std::make_unique<SpecializeLambdaSetsPass>();
}

void registerSpecializeLambdaSetsPass() {
  mlir::PassRegistration<SpecializeLambdaSetsPass>();
}

} // namespace sammine_lang
