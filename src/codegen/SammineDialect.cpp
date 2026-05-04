#include "codegen/SammineDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"

#include "SammineDialect.cpp.inc"

#define GET_OP_CLASSES
#include "SammineOps.cpp.inc"

namespace sammine_lang::smn {

void SammineDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "SammineOps.cpp.inc"
      >();
}

} // namespace sammine_lang::smn
