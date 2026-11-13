#include "dialect/ElaborateDrops.h"
#include "dialect/OwnershipAnalysis.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Dominance.h"

#include "llvm/ADT/DenseMap.h"

//! \file ElaborateDrops.cpp
//! \brief Where each value still owned goes out of use, from the ownership
//! analysis' states, and the drops put there.

namespace sammine_lang {

bool dropsOnItsOwn(mlir::Type type) {
  auto ptr = mlir::dyn_cast<smn::PtrType>(type);
  return ptr && ptr.isOwned() &&
         (!ptr.isBox() || !smn::ownsMemory(ptr.getPointee()));
}

namespace {

using namespace ownership;

class Elaboration {
public:
  explicit Elaboration(mlir::func::FuncOp func)
      : func_(func), flow_(func), dominance_(func) {}

  void run() {
    if (mlir::failed(flow_.run()))
      return;
    number();
    collectReturns();
    collectEdges();
    for (auto &[ret, paths] : atReturns_) {
      mlir::OpBuilder builder(ret);
      dropAll(builder, ret->getLoc(), paths);
    }
    for (auto &edge : onEdges_)
      dropOnEdge(edge);
  }

private:
  struct Edge {
    mlir::Operation *terminator;
    unsigned successor;
    llvm::SmallVector<Path> paths;
  };

  mlir::func::FuncOp func_;
  OwnershipFlow flow_;
  mlir::DominanceInfo dominance_;
  /// Each value in the order the function defines it: drops go in reverse.
  llvm::DenseMap<mlir::Value, unsigned> order_;
  llvm::SmallVector<std::pair<mlir::Operation *, llvm::SmallVector<Path>>>
      atReturns_;
  llvm::SmallVector<Edge> onEdges_;

  void number() {
    unsigned next = 0;
    func_.walk([&](mlir::Block *block) {
      for (auto arg : block->getArguments())
        order_[arg] = next++;
      for (auto &op : *block)
        for (auto result : op.getResults())
          order_[result] = next++;
    });
  }

  /// The parts of `owners` live at `at`, that elaboration drops, those
  /// `keep` accepts, last bound first. Only of values defined on every path
  /// to `at`: a state keeps a value defined on one side of a join past it,
  /// where nothing could drop it.
  llvm::SmallVector<Path>
  droppable(const Owners &owners, mlir::Operation *at,
            llvm::function_ref<bool(const Path &)> keep) {
    llvm::SmallVector<Path> roots, live, paths;
    for (auto &[path, owner] : owners)
      if (path.steps.empty())
        roots.push_back(path);
    for (auto &root : roots)
      liveOwners(owners, root, live);
    for (auto &path : live)
      if (owners.at(path).kind == Owner::Live && drops(owners, path) &&
          dominance_.dominates(path.root, at) && keep(path))
        paths.push_back(path);
    llvm::sort(paths, [&](const Path &a, const Path &b) {
      if (a.root != b.root)
        return order_.lookup(a.root) > order_.lookup(b.root);
      return b < a;
    });
    return paths;
  }

  /// Whether elaboration drops `path` in `owners`: a whole value, not a
  /// part of one, whose type drops on its own, or a box what it points at
  /// was moved out of, whose drop is a free. Not a part: a struct's drop is
  /// its own, a field at a time or through its Drop instance (steps 8 and
  /// 9), and an io::File's handle is closed, not freed.
  static bool drops(const Owners &owners, const Path &path) {
    if (!path.steps.empty())
      return false;
    auto type = typeOf(path);
    if (dropsOnItsOwn(type))
      return true;
    auto ptr = mlir::dyn_cast<smn::PtrType>(type);
    if (!ptr || !ptr.isBox())
      return false;
    auto pointee = owners.find(path.child({Step::Pointee}));
    return pointee == owners.end() || pointee->second.kind == Owner::Moved;
  }

  /// What is still owned at each return, once it has taken what it
  /// returns.
  void collectReturns() {
    func_.walk([&](mlir::func::ReturnOp ret) {
      if (!flow_.reaches(ret->getBlock()))
        return;
      auto *before = flow_.before(ret);
      if (!before)
        return;
      auto owners = *before;
      applyOp(owners, ret, nullptr);
      auto paths = droppable(owners, ret, [](const Path &) { return true; });
      if (!paths.empty())
        atReturns_.push_back({ret, std::move(paths)});
    });
  }

  /// What each edge leaves owned that isn't where its block starts: moved
  /// on another edge, or past where its value is defined.
  void collectEdges() {
    func_.walk([&](mlir::Operation *op) {
      auto branch = mlir::dyn_cast<mlir::BranchOpInterface>(op);
      if (!branch || !flow_.reaches(op->getBlock()))
        return;
      auto *before = flow_.before(op);
      if (!before)
        return;
      for (unsigned i = 0; i < op->getNumSuccessors(); ++i) {
        auto *block = op->getSuccessor(i);
        auto *entry = flow_.before(&block->front());
        auto owners = *before;
        applyEdge(owners, branch, i, nullptr);
        auto paths = droppable(owners, op, [&](const Path &path) {
          if (!dominance_.dominates(path.root, &block->front()))
            return true;
          if (!entry)
            return false;
          auto it = entry->find(path);
          return it != entry->end() && it->second.kind != Owner::Live;
        });
        if (!paths.empty())
          onEdges_.push_back({op, i, std::move(paths)});
      }
    });
  }

  /// The value `path` names, made where `builder` is: the root, a slot
  /// moved out of, a field extracted, a pointee or a field of a place moved
  /// out of.
  mlir::Value valueOf(mlir::OpBuilder &builder, mlir::Location loc,
                      const Path &path) {
    mlir::Value current = path.root;
    for (auto &step : path.steps) {
      if (step.kind == Step::Pointee) {
        // Through the pointer, which stays where it is.
        auto type = current.getType();
        auto place = mlir::dyn_cast<smn::PlaceType>(type);
        auto ptr =
            mlir::cast<smn::PtrType>(place ? place.getValueType() : type);
        auto pointee =
            smn::PlaceType::get(builder.getContext(), ptr.getPointee());
        current =
            place ? smn::ThroughOp::create(builder, loc, pointee, current)
                        .getResult()
                  : smn::DerefOp::create(builder, loc, pointee, current)
                        .getResult();
        continue;
      }
      auto index = static_cast<uint64_t>(step.a);
      if (auto place = mlir::dyn_cast<smn::PlaceType>(current.getType())) {
        auto field = smn::fieldTypes(place.getValueType());
        current = smn::FieldOp::create(
            builder, loc,
            smn::PlaceType::get(builder.getContext(), (*field)[index]),
            current, builder.getI64IntegerAttr(static_cast<int64_t>(index)));
        continue;
      }
      auto field = smn::fieldTypes(current.getType());
      current = smn::ExtractOp::create(builder, loc, (*field)[index], current,
                                       index);
    }
    if (auto place = mlir::dyn_cast<smn::PlaceType>(current.getType()))
      current =
          smn::MoveOp::create(builder, loc, place.getValueType(), current);
    return current;
  }

  void dropAll(mlir::OpBuilder &builder, mlir::Location loc,
               llvm::ArrayRef<Path> paths) {
    for (auto &path : paths)
      smn::DropOp::create(builder, loc, valueOf(builder, loc, path),
                          builder.getUnitAttr());
  }

  /// The drops of `edge`: before its branch if the branch goes nowhere
  /// else, else in a block of their own on the edge.
  void dropOnEdge(Edge &edge) {
    auto *op = edge.terminator;
    auto loc = op->getLoc();
    if (op->getNumSuccessors() == 1) {
      mlir::OpBuilder builder(op);
      dropAll(builder, loc, edge.paths);
      return;
    }
    auto branch = mlir::cast<mlir::BranchOpInterface>(op);
    auto *target = op->getSuccessor(edge.successor);
    auto operands = branch.getSuccessorOperands(edge.successor);
    llvm::SmallVector<mlir::Value> forwarded(operands.getForwardedOperands());
    operands.erase(0, operands.size());
    auto *block = new mlir::Block();
    target->getParent()->getBlocks().insertAfter(
        mlir::Region::iterator(op->getBlock()), block);
    op->setSuccessor(block, edge.successor);
    mlir::OpBuilder builder = mlir::OpBuilder::atBlockEnd(block);
    dropAll(builder, loc, edge.paths);
    mlir::cf::BranchOp::create(builder, loc, target, forwarded);
  }
};

struct ElaborateDropsPass
    : public mlir::PassWrapper<ElaborateDropsPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ElaborateDropsPass)

  llvm::StringRef getArgument() const override {
    return "sammine-elaborate-drops";
  }
  llvm::StringRef getDescription() const override {
    return "Drop each value that owns memory where it goes out of use";
  }
  void runOnOperation() override { elaborateDrops(getOperation()); }
};

} // namespace

void elaborateDrops(mlir::ModuleOp module) {
  for (auto func : module.getOps<mlir::func::FuncOp>())
    if (!func.isDeclaration() && func->hasAttr(smn::kMirAttr))
      Elaboration(func).run();
}

std::unique_ptr<mlir::Pass> createElaborateDropsPass() {
  return std::make_unique<ElaborateDropsPass>();
}

void registerElaborateDropsPass() {
  mlir::PassRegistration<ElaborateDropsPass>();
}

} // namespace sammine_lang
