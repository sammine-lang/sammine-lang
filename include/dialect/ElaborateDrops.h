#pragma once

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include <memory>

//! \file ElaborateDrops.h
//! \brief Drops each value that owns memory where it goes out of use, so the
//! program needn't: at each return, what is still owned, and on each edge
//! into a block, what is owned at the end of the edge but not where the
//! block starts (moved on another edge, or defined only on this side). The
//! ownership analysis' states say which, so no runtime flags are needed.
//! Each drop it inserts is `sammine.drop ... implicit`. See
//! knowledge_base/plans/drops.md.

namespace sammine_lang {

/// The function that drops a value of `type`, its Drop instance's method
/// (`Drop<T>::Drop`) in `module`, or null if it has none.
mlir::func::FuncOp dropFunction(mlir::ModuleOp module, mlir::Type type);

/// Each drop of a struct, tuple or enum becomes a call of its type's Drop
/// instance, or, with none, the drops of its parts: a struct's or tuple's
/// fields, an enum's payload, the variant its tag picks, an array's
/// elements, in a loop. What is left drops pointers, which lower to free.
/// Run before lowering to LLVM.
void expandDrops(mlir::ModuleOp module);

/// Elaborates the drops of every function MIR defines (marked
/// `sammine.mir`) in `module`.
void elaborateDrops(mlir::ModuleOp module);

/// `sammine-elaborate-drops`: elaborateDrops as a pass, for sammine-opt.
std::unique_ptr<mlir::Pass> createElaborateDropsPass();

/// Makes the pass available to tools by its name.
void registerElaborateDropsPass();

} // namespace sammine_lang
