#pragma once

#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "SammineDialect.h.inc"

#define GET_OP_CLASSES
#include "SammineOps.h.inc"
