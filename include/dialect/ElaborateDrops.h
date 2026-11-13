#pragma once

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

/// Whether elaboration drops a value of MIR type `type` where it goes out
/// of use. So far: an owned pointer whose drop only frees its memory, a
/// buffer, or a box of a value that owns nothing.
bool dropsOnItsOwn(mlir::Type type);

/// Elaborates the drops of every function MIR defines (marked
/// `sammine.mir`) in `module`.
void elaborateDrops(mlir::ModuleOp module);

/// `sammine-elaborate-drops`: elaborateDrops as a pass, for sammine-opt.
std::unique_ptr<mlir::Pass> createElaborateDropsPass();

/// Makes the pass available to tools by its name.
void registerElaborateDropsPass();

} // namespace sammine_lang
