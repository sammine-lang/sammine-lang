#pragma once

#include "typecheck/Types.h"

#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <optional>
#include <vector>

//! \file ClosureLayouts.h
//! \brief How many words of bound values a value of each function type
//! carries: the most any closure of that type the unit makes binds. A
//! closure carries what it binds, by value (see
//! knowledge_base/plans/closures-by-value.md).

namespace sammine_lang {

class MIRTypes;

class ClosureLayouts {
public:
  explicit ClosureLayouts(mlir::MLIRContext *ctx);
  ~ClosureLayouts();

  /// A closure the unit makes, of function type `closure`, binding values of
  /// the types `bound`: a partial application.
  void addClosure(const Type &closure, std::vector<Type> bound);

  /// The words a value of the function type `fn` carries, the MIR types of
  /// what closures bind given by `types`. A closure that binds a value of
  /// its own type, directly or through other closures, can't carry it: its
  /// size would have no end. It carries a pointer to what it binds, in its
  /// maker's frame, and stays there as a view does. Nullopt only while `fn`
  /// is being sized, to whoever asks for it from inside.
  std::optional<int64_t> words(const Type &fn, const MIRTypes &types) const;

private:
  /// What identifies a function type: its MIR type with every function
  /// type in it carrying no words, so finding it needs no other type's.
  mlir::Type shapeOf(const Type &fn) const;

  std::unique_ptr<MIRTypes> shapes_;
  /// Per shape: what each of its closures binds.
  llvm::DenseMap<mlir::Type, std::vector<std::vector<Type>>> closures_;
  mutable llvm::DenseMap<mlir::Type, int64_t> words_;
  mutable llvm::DenseSet<mlir::Type> sizing_;
};

} // namespace sammine_lang
