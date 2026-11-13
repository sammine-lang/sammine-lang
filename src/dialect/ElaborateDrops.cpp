#include "dialect/ElaborateDrops.h"
#include "dialect/OwnershipAnalysis.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Dominance.h"

#include "llvm/ADT/DenseMap.h"

//! \file ElaborateDrops.cpp
//! \brief Where each value still owned goes out of use, from the ownership
//! analysis' states, and the drops put there.

namespace sammine_lang {

void expandDrops(mlir::ModuleOp module) {
  llvm::SmallVector<smn::DropOp> work;
  module.walk([&](smn::DropOp drop) { work.push_back(drop); });
  while (!work.empty()) {
    auto drop = work.pop_back_val();
    auto value = drop.getValue();
    auto type = value.getType();
    auto loc = drop.getLoc();
    mlir::OpBuilder builder(drop);
    if (auto fn = smn::dropFunction(module, type)) {
      smn::CallOp::create(builder, loc, mlir::TypeRange{}, fn.getName(),
                          value);
    } else if (auto fields = smn::fieldTypes(type)) {
      for (auto [i, field] : llvm::enumerate(*fields))
        if (smn::ownsMemory(field))
          work.push_back(smn::DropOp::create(
              builder, loc,
              smn::ExtractOp::create(builder, loc, field, value, i),
              drop.getImplicitAttr()));
    } else if (auto en = mlir::dyn_cast<smn::EnumType>(type)) {
      // Its tag picks the variant, whose payload's fields are dropped; one
      // that owns nothing goes straight on.
      auto *after =
          drop->getBlock()->splitBlock(std::next(drop->getIterator()));
      auto tag = smn::TagOp::create(builder, loc, builder.getI32Type(), value);
      llvm::SmallVector<int32_t> tags;
      llvm::SmallVector<mlir::Block *> dests;
      for (uint64_t v = 0; v < en.getVariants().size(); ++v) {
        auto payload = en.getPayload(v);
        if (!llvm::any_of(payload, smn::ownsMemory))
          continue;
        auto *block = new mlir::Block();
        after->getParent()->getBlocks().insert(after->getIterator(), block);
        tags.push_back(static_cast<int32_t>(v));
        dests.push_back(block);
        mlir::OpBuilder inArm = mlir::OpBuilder::atBlockEnd(block);
        auto offsets = smn::payloadOffsets(payload);
        for (auto [i, field] : llvm::enumerate(payload))
          if (smn::ownsMemory(field))
            work.push_back(smn::DropOp::create(
                inArm, loc,
                smn::PayloadOp::create(
                    inArm, loc, field, value,
                    inArm.getI64IntegerAttr(static_cast<int64_t>(v)),
                    inArm.getI64IntegerAttr(static_cast<int64_t>(i)),
                    inArm.getI64IntegerAttr(offsets[i])),
                drop.getImplicitAttr()));
        mlir::cf::BranchOp::create(inArm, loc, after);
      }
      llvm::SmallVector<mlir::ValueRange> noOperands(dests.size());
      mlir::cf::SwitchOp::create(builder, loc, tag, after, mlir::ValueRange{},
                                 tags, dests, noOperands);
    } else if (auto array = mlir::dyn_cast<smn::ArrayType>(type)) {
      // Each element, in a loop: i from 0 while i < N, dropping element i.
      auto *after =
          drop->getBlock()->splitBlock(std::next(drop->getIterator()));
      auto *region = after->getParent();
      auto i64 = builder.getI64Type();
      auto *header = new mlir::Block();
      auto *body = new mlir::Block();
      region->getBlocks().insert(after->getIterator(), header);
      region->getBlocks().insert(after->getIterator(), body);
      auto index = header->addArgument(i64, loc);
      auto constant = [&](mlir::OpBuilder &b, int64_t v) {
        return mlir::arith::ConstantIntOp::create(b, loc, i64, v).getResult();
      };
      mlir::cf::BranchOp::create(builder, loc, header,
                                 mlir::ValueRange{constant(builder, 0)});
      mlir::OpBuilder inHeader = mlir::OpBuilder::atBlockEnd(header);
      auto more = mlir::arith::CmpIOp::create(
          inHeader, loc, mlir::arith::CmpIPredicate::slt, index,
          constant(inHeader, array.getSize()));
      mlir::cf::CondBranchOp::create(inHeader, loc, more, body, after);
      mlir::OpBuilder inBody = mlir::OpBuilder::atBlockEnd(body);
      work.push_back(smn::DropOp::create(
          inBody, loc,
          smn::ElementOp::create(inBody, loc, array.getElement(), value, index),
          drop.getImplicitAttr()));
      auto next =
          mlir::arith::AddIOp::create(inBody, loc, index, constant(inBody, 1));
      mlir::cf::BranchOp::create(inBody, loc, header,
                                 mlir::ValueRange{next.getResult()});
    } else {
      continue;
    }
    drop.erase();
  }
}

namespace {

using namespace ownership;

class Elaboration {
public:
  explicit Elaboration(mlir::func::FuncOp func)
      : func_(func), module_(func->getParentOfType<mlir::ModuleOp>()),
        flow_(func), dominance_(func) {}

  void run() {
    if (mlir::failed(flow_.run()))
      return;
    number();
    collectReturns();
    collectEnds();
    collectEdges();
    for (auto &[op, paths] : before_) {
      mlir::OpBuilder builder(op);
      dropAll(builder, op->getLoc(), paths);
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
  mlir::ModuleOp module_;
  OwnershipFlow flow_;
  mlir::DominanceInfo dominance_;
  /// Each value in the order the function defines it: drops go in reverse.
  llvm::DenseMap<mlir::Value, unsigned> order_;
  /// What to drop before an op: a return, a box's drop, a store.
  llvm::SmallVector<std::pair<mlir::Operation *, llvm::SmallVector<Path>>>
      before_;
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

  /// The parts of `owners` live at `at` to drop, those `keep` accepts,
  /// last bound first, a pointee before its box. Only of values defined on
  /// every path to `at`: a state keeps a value defined on one side of a
  /// join past it, where nothing could drop it.
  llvm::SmallVector<Path>
  droppable(const Owners &owners, mlir::Operation *at,
            llvm::function_ref<bool(const Path &)> keep) {
    llvm::SmallVector<Path> live, paths;
    for (auto &[path, owner] : owners)
      if (path.steps.empty() && dominance_.dominates(path.root, at))
        collect(owners, path, live);
    for (auto &path : live)
      if (keep(path))
        paths.push_back(path);
    sort(paths);
    return paths;
  }

  /// Last bound first, a pointee before its box.
  void sort(llvm::SmallVectorImpl<Path> &paths) {
    llvm::sort(paths, [&](const Path &a, const Path &b) {
      if (a.root != b.root)
        return order_.lookup(a.root) > order_.lookup(b.root);
      return b < a;
    });
  }

  /// What an op ends that is still live: before the program drops a box,
  /// what it points at; before a store, what the place holds.
  void collectEnds() {
    func_.walk([&](mlir::Operation *op) {
      std::optional<Path> ended;
      if (auto drop = mlir::dyn_cast<smn::DropOp>(op);
          drop && !drop.getImplicit()) {
        auto ptr = mlir::dyn_cast<smn::PtrType>(drop.getValue().getType());
        if (auto path = pathOf(drop.getValue()); path && ptr && ptr.isBox())
          ended = path->child({Step::Pointee});
      } else if (auto store = mlir::dyn_cast<smn::StoreOp>(op)) {
        ended = pathOf(store.getPlace());
      }
      if (!ended || !flow_.reaches(op->getBlock()))
        return;
      auto *before = flow_.before(op);
      if (!before)
        return;
      llvm::SmallVector<Path> paths;
      collect(*before, *ended, paths);
      sort(paths);
      if (!paths.empty())
        before_.push_back({op, std::move(paths)});
    });
  }

  /// What of `path` is live in `owners` and is dropped, as its type drops:
  /// a pointer, after what a box points at; a value with a Drop instance,
  /// an enum or an array, whole, if all of it is there; any other struct or
  /// tuple, field by field. Inside T's own Drop, a T is dropped field by
  /// field, or it would call itself.
  void collect(const Owners &owners, const Path &path,
               llvm::SmallVectorImpl<Path> &out) {
    auto it = owners.find(path);
    if (it == owners.end() || it->second.kind != Owner::Live)
      return;
    auto type = typeOf(path);
    if (auto ptr = mlir::dyn_cast<smn::PtrType>(type)) {
      if (ptr.isBox())
        collect(owners, path.child({Step::Pointee}), out);
      out.push_back(path);
      return;
    }
    if (auto drop = smn::dropFunction(module_, type);
        drop && drop.getOperation() != func_.getOperation()) {
      if (allLive(owners, path))
        out.push_back(path);
      return;
    }
    // An enum drops whole, its tag picking the payload; one a case took
    // apart, its arms' bindings drop. An array drops whole, element by
    // element: its elements move only with it.
    if (mlir::isa<smn::EnumType, smn::ArrayType>(type)) {
      if (allLive(owners, path))
        out.push_back(path);
      return;
    }
    if (!smn::fieldTypes(type))
      return;
    for (auto step : partsOf(type))
      collect(owners, path.child(step), out);
  }

  static bool allLive(const Owners &owners, const Path &path) {
    for (auto &[part, owner] : owners)
      if (path.contains(part) && owner.kind != Owner::Live)
        return false;
    return true;
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
        before_.push_back({ret, std::move(paths)});
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
