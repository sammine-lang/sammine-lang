#pragma once

#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "SammineDialect.h.inc"

#include "SammineEnums.h.inc"

#define GET_TYPEDEF_CLASSES
#include "SammineTypes.h.inc"

namespace sammine_lang::smn {
/// Whether a value of `type` owns memory, which its holder must free or hand
/// on: an owned pointer, so far.
bool ownsMemory(mlir::Type type);
} // namespace sammine_lang::smn

#define GET_OP_CLASSES
#include "SammineOps.h.inc"
