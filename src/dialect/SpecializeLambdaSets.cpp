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
//! The unit copied is an SCC of the call graph, the functions calling each
//! other (the paper's 6.3): a copy of it calls its own copies, so their
//! sets are the copy's. Most are one function, which then calls itself in
//! its own copy.
//!
//! SCCs are taken callers first. Each place that calls a function of a
//! private SCC taking or giving function values, or makes a closure of it,
//! gets its own copy of the SCC; when the SCC's turn comes, all its callers
//! are done, and its copies whose signatures have alike sets
//! (describeSignatures) are merged before their own places are copied, so
//! copies follow the sets an SCC is used with, not every path to it. The
//! SCCs not yet taken are opaque to that description: their places, all
//! naming the one SCC yet, would mix the sets of each. Once all are taken,
//! copies are merged again, with every call followed, until none are alike.

namespace sammine_lang {

namespace {

using Func = mlir::func::FuncOp;

/// The most ops copies may add, so a long chain of higher-order calls
/// doesn't grow the module without end: past it, places share an SCC, as
/// without this step.
constexpr int64_t kCopyBudget = 100000;

/// The function a call or a closure names, or null for any other op.
mlir::FlatSymbolRefAttr calleeOf(mlir::Operation *op) {
  if (auto call = mlir::dyn_cast<smn::CallOp>(op))
    return call.getCalleeAttr();
  if (auto closure = mlir::dyn_cast<smn::ClosureOp>(op))
    return closure.getCalleeAttr();
  return {};
}

/// An SCC's functions, or a copy's: member i of a copy copies member i.
using Group = llvm::SmallVector<Func, 1>;

bool inGroup(const Group &group, mlir::Operation *op) {
  return llvm::any_of(group, [&](Func f) { return f->isAncestor(op); });
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
  /// Whether anything outside `group` names one of its functions.
  bool usedOutside(const Group &group) {
    return llvm::any_of(group, [&](Func func) {
      auto it = by_.find(func.getName());
      return it != by_.end() &&
             llvm::any_of(it->second, [&](mlir::Operation *site) {
               return !inGroup(group, site);
             });
    });
  }

private:
  llvm::StringMap<llvm::SetVector<mlir::Operation *>> by_;
};

class Specializer {
public:
  explicit Specializer(mlir::ModuleOp module)
      : module_(module), symbols_(module) {}

  void run() {
    findNamedOtherwise();
    findSccs();
    sites_.addIn(module_);
    copy();
    for (bool merged = true; merged;) {
      eraseUnused();
      merged = false;
      auto described = describeSignatures(module_);
      for (unsigned scc = 0; scc < copies_.size(); ++scc)
        merged |= mergeCopies(scc, described);
    }
  }

private:
  mlir::ModuleOp module_;
  mlir::SymbolTable symbols_;
  /// The SCCs of the call graph among the functions MIR defines, callees
  /// before callers.
  std::vector<Group> sccs_;
  /// Per function, copies too: its SCC, an index into sccs_ and copies_,
  /// and which member of it it is.
  llvm::DenseMap<mlir::Operation *, std::pair<unsigned, unsigned>> member_;
  /// Per SCC: it, then its copies.
  std::vector<llvm::SmallVector<Group>> copies_;
  /// The SCCs that were copied.
  llvm::DenseSet<unsigned> copied_;
  /// Functions something other than a call or a closure names: copies
  /// wouldn't be found from there.
  llvm::DenseSet<mlir::StringAttr> namedOtherwise_;
  Sites sites_;
  int64_t budget_ = kCopyBudget;
  unsigned numbered_ = 0;

  static bool defined(Func func) {
    return func && !func.isDeclaration() && func->hasAttr(smn::kMirAttr);
  }
  Func lookup(mlir::FlatSymbolRefAttr name) {
    auto func = symbols_.lookup<Func>(name.getValue());
    return defined(func) ? func : Func();
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

  /// Whether a copy of SCC `scc` per place could have other sets: one of
  /// its functions takes or gives function values, and only this module's
  /// calls and closures name them.
  bool specializable(unsigned scc) {
    bool takesFunctions = false;
    for (auto func : sccs_[scc]) {
      // A Drop instance is also found by its name.
      if (!func.isPrivate() || func.getName().starts_with("Drop<") ||
          namedOtherwise_.count(func.getNameAttr()))
        return false;
      auto type = func.getFunctionType();
      takesFunctions |= llvm::any_of(type.getInputs(), holdsFunctionValue) ||
                        llvm::any_of(type.getResults(), holdsFunctionValue);
    }
    return takesFunctions;
  }

  void findNamedOtherwise();
  void findSccs();
  Group copyOf(unsigned scc);
  void copy();
  bool mergeCopies(unsigned scc,
                   std::map<mlir::Operation *, std::string> &described);
  void erase(const Group &group);
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
    Group scc;
    Func member;
    do {
      member = stack.pop_back_val();
      onStack.erase(member);
      member_[member] = {static_cast<unsigned>(sccs_.size()),
                         static_cast<unsigned>(scc.size())};
      scc.push_back(member);
    } while (member != func);
    sccs_.push_back(scc);
    copies_.push_back({scc});
  };
  for (auto func : module_.getOps<Func>())
    if (defined(func) && !index.count(func))
      visit(func);
}

Group Specializer::copyOf(unsigned scc) {
  // Every member is copied, and named apart by one number.
  auto &original = sccs_[scc];
  ++numbered_;
  Group copy;
  for (auto [i, func] : llvm::enumerate(original)) {
    auto clone = func.clone();
    std::string name;
    do
      name = (func.getName() + ".spec" + llvm::Twine(numbered_)).str();
    while (symbols_.lookup(name));
    clone.setName(name);
    symbols_.insert(clone, std::next(mlir::Block::iterator(func)));
    member_[clone] = {scc, static_cast<unsigned>(i)};
    func.walk([&](mlir::Operation *) { --budget_; });
    copy.push_back(clone);
  }
  // Calling a member, a copy calls its own copy of it.
  for (auto clone : copy) {
    sites_.addIn(clone);
    for (auto *site : sitesIn(clone))
      for (auto [i, func] : llvm::enumerate(original))
        if (calleeOf(site).getValue() == func.getName())
          sites_.retarget(site, copy[i].getNameAttr());
  }
  copied_.insert(scc);
  copies_[scc].push_back(copy);
  return copy;
}

void Specializer::copy() {
  // An SCC keeps its first place; each other place gets a copy.
  llvm::DenseSet<unsigned> placed;
  // The functions to be copied whose turn hasn't come.
  llvm::StringSet<> waiting;
  for (unsigned scc = 0; scc < sccs_.size(); ++scc)
    if (specializable(scc))
      for (auto func : sccs_[scc])
        waiting.insert(func.getName());

  for (unsigned scc = static_cast<unsigned>(sccs_.size()); scc-- > 0;) {
    for (auto func : sccs_[scc])
      waiting.erase(func.getName());
    if (copies_[scc].size() > 1) {
      auto described = describeSignatures(module_, waiting);
      mergeCopies(scc, described);
    }
    auto groups = copies_[scc];
    for (auto &group : groups)
      for (auto from : group)
        for (auto *site : sitesIn(from)) {
          auto [callee, which] = member_.lookup(lookup(calleeOf(site)));
          if (callee == scc || !specializable(callee) ||
              placed.insert(callee).second || budget_ <= 0)
            continue;
          sites_.retarget(site, copyOf(callee)[which].getNameAttr());
        }
  }
}

void Specializer::erase(const Group &group) {
  for (auto func : group)
    sites_.removeIn(func);
  for (auto func : group)
    symbols_.erase(func);
}

bool Specializer::mergeCopies(
    unsigned scc, std::map<mlir::Operation *, std::string> &described) {
  auto &all = copies_[scc];
  if (all.size() < 2)
    return false;
  bool merged = false;
  std::map<std::string, Group> kept;
  llvm::SmallVector<Group> alive;
  for (auto &group : all) {
    std::string key;
    for (auto func : group)
      key += described[func] + ";";
    auto [it, fresh] = kept.try_emplace(key, group);
    if (fresh) {
      alive.push_back(group);
      continue;
    }
    // Its own places go with it; others name the kept copy's members.
    for (auto func : group)
      sites_.removeIn(func);
    for (auto [func, into] : llvm::zip(group, it->second))
      sites_.redirect(func, into);
    for (auto func : group)
      symbols_.erase(func);
    merged = true;
  }
  all = std::move(alive);
  return merged;
}

void Specializer::eraseUnused() {
  for (bool erased = true; erased;) {
    erased = false;
    for (unsigned scc = 0; scc < copies_.size(); ++scc) {
      // Only an SCC that was copied, and its copies: others are as the
      // program had them.
      if (!copied_.count(scc))
        continue;
      llvm::SmallVector<Group> alive;
      for (auto &group : copies_[scc]) {
        if (!sites_.usedOutside(group)) {
          erase(group);
          erased = true;
        } else {
          alive.push_back(group);
        }
      }
      copies_[scc] = std::move(alive);
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
