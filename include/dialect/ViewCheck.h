#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LogicalResult.h"

#include <memory>

//! \file ViewCheck.h
//! \brief The rules on views, ptr<T>, that MIR checks op by op: nothing is
//! written through a view, and `&` makes no view of what an owned pointer
//! points at, nor of an owned pointer itself.

namespace sammine_lang {

/// Checks every function MIR defines (marked `sammine.mir`) in `module`,
/// reporting each error as an MLIR diagnostic. Failure if any was reported.
mlir::LogicalResult checkViews(mlir::ModuleOp module);

/// `sammine-check-views`: checkViews as a pass, for sammine-opt.
std::unique_ptr<mlir::Pass> createCheckViewsPass();

/// Makes the pass available to tools by its name.
void registerCheckViewsPass();

} // namespace sammine_lang
