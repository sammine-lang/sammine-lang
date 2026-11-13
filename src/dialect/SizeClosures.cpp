#include "dialect/SizeClosures.h"
#include "dialect/SammineDialect.h"

#include "mlir/IR/AttrTypeSubElements.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"

#include <optional>

namespace sammine_lang {

namespace {

/// `fn` carrying `words`.
mlir::Type withWords(smn::FnType fn, int64_t words) {
  return smn::FnType::get(fn.getContext(), fn.getSignature(), words);
}

/// What identifies a function type: it with every function type in it
/// carrying no words, so finding it needs no other type's size.
mlir::Type shapeOf(mlir::Type type) {
  return smn::mapType(type, [](mlir::Type t) {
    auto fn = mlir::dyn_cast<smn::FnType>(t);
    return fn ? withWords(fn, 0) : t;
  });
}

class ClosureSizes {
public:
  explicit ClosureSizes(mlir::ModuleOp module) {
    module.walk([&](smn::ClosureOp closure) {
      closures_[shapeOf(closure.getType())].push_back(
          llvm::to_vector(closure.getBound().getTypes()));
    });
  }

  /// `type` with each function type in it carrying its words. Null only
  /// while a function type in it is being sized, to whoever asks from
  /// inside.
  mlir::Type sized(mlir::Type type) {
    return smn::mapType(type, [&](mlir::Type t) -> mlir::Type {
      auto fn = mlir::dyn_cast<smn::FnType>(t);
      if (!fn)
        return t;
      auto carried = words(shapeOf(fn));
      return carried ? withWords(fn, *carried) : mlir::Type();
    });
  }

private:
  /// Per shape: what each closure of it binds.
  llvm::MapVector<mlir::Type, std::vector<llvm::SmallVector<mlir::Type>>>
      closures_;
  llvm::DenseMap<mlir::Type, int64_t> words_;
  llvm::DenseSet<mlir::Type> sizing_;

  /// The words a value of the function type of `shape` carries, or nullopt
  /// while it is being sized.
  std::optional<int64_t> words(mlir::Type shape) {
    if (auto known = words_.find(shape); known != words_.end())
      return known->second;
    auto closures = closures_.find(shape);
    if (closures == closures_.end())
      return 0;
    // Sizing this type needs this type's size: a closure of it binds one,
    // or binds one through other closures. The closure that meets the cycle
    // keeps what it binds in its maker's frame, and carries a pointer.
    if (!sizing_.insert(shape).second)
      return std::nullopt;
    int64_t most = 0;
    for (auto &bound : closures->second) {
      auto carried = sized(smn::TupleType::get(shape.getContext(), bound));
      most = std::max(most, carried ? smn::wordsFor(carried) : 1);
    }
    sizing_.erase(shape);
    words_[shape] = most;
    return most;
  }
};

struct SizeClosuresPass
    : public mlir::PassWrapper<SizeClosuresPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SizeClosuresPass)

  llvm::StringRef getArgument() const override {
    return "sammine-size-closures";
  }
  llvm::StringRef getDescription() const override {
    return "Give each function type the words its closures bind";
  }
  void runOnOperation() override { sizeClosures(getOperation()); }
};

} // namespace

void sizeClosures(mlir::ModuleOp module) {
  ClosureSizes sizes(module);
  // Every type in the module: of values, block arguments and signatures.
  mlir::AttrTypeReplacer replacer;
  replacer.addReplacement(
      [&](mlir::Type type)
          -> std::optional<std::pair<mlir::Type, mlir::WalkResult>> {
        auto sized = sizes.sized(type);
        if (sized == type)
          return std::nullopt;
        return std::pair{sized, mlir::WalkResult::skip()};
      });
  replacer.recursivelyReplaceElementsIn(module, /*replaceAttrs=*/true,
                                        /*replaceLocs=*/false,
                                        /*replaceTypes=*/true);
  smn::refreshPayloadOffsets(module);
}

std::unique_ptr<mlir::Pass> createSizeClosuresPass() {
  return std::make_unique<SizeClosuresPass>();
}

void registerSizeClosuresPass() { mlir::PassRegistration<SizeClosuresPass>(); }

} // namespace sammine_lang
