#pragma once

#include "mlir/IR/BuiltinOps.h"

namespace sammine_lang {

/// Lower linalg.reduce ops in the module to gpu.launch + gpu.all_reduce.
/// Should run after bufferization (memref types) and before
/// linalg-to-parallel-loops (which only handles parallel iterators).
mlir::LogicalResult lowerLinalgReduceToGpuLaunch(mlir::ModuleOp module);

} // namespace sammine_lang
