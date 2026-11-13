#pragma once

#include "typecheck/Types.h"

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/MLIRContext.h"

#include <optional>

//! \file MIRTypes.h
//! \brief The MIR type of each sammine type (see the table in
//! knowledge_base/codegen.md).

namespace sammine_lang {

class ClosureLayouts;

class MIRTypes {
public:
  explicit MIRTypes(mlir::MLIRContext *ctx) : ctx_(ctx) {}

  /// Function types carry as many words as `layouts` says a closure of
  /// theirs binds. Without layouts they carry none: the type's shape.
  void setClosureLayouts(const ClosureLayouts *layouts) { layouts_ = layouts; }

  /// The MIR type of `type`, or nullopt for one MIR has none for: (), a
  /// type parameter, a C function's `...`.
  std::optional<mlir::Type> mirType(const Type &type) const;
  /// A function type in MIR types, if MIR has a type for each part. A C
  /// function's `...` has none: this is what it declares. A function
  /// returning () returns nothing.
  std::optional<mlir::FunctionType>
  mirFunctionType(const FunctionType &fn) const;
  /// The MIR types of `types`, in order, if MIR has one for each.
  std::optional<llvm::SmallVector<mlir::Type>>
  mirTypes(llvm::ArrayRef<Type> types) const;

private:
  mlir::MLIRContext *ctx_;
  const ClosureLayouts *layouts_ = nullptr;
};

} // namespace sammine_lang
