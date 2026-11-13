#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LogicalResult.h"

#include <memory>

//! \file OwnershipCheck.h
//! \brief The ownership check on MIR: a value that owns memory is used at
//! most once on every path, and none is still owned when its function
//! returns.

namespace sammine_lang {

/// Checks every function MIR defines (marked `sammine.mir`) in `module`,
/// reporting each error as an MLIR diagnostic. Failure if any was reported.
mlir::LogicalResult checkOwnership(mlir::ModuleOp module);

/// `sammine-check-ownership`: checkOwnership as a pass, for sammine-opt.
std::unique_ptr<mlir::Pass> createCheckOwnershipPass();

/// Makes the pass available to tools by its name.
void registerCheckOwnershipPass();

} // namespace sammine_lang
