#include "codegen/SammineDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

#include "SammineDialect.cpp.inc"

#include "SammineEnums.cpp.inc"

#define GET_TYPEDEF_CLASSES
#include "SammineTypes.cpp.inc"

#define GET_OP_CLASSES
#include "SammineOps.cpp.inc"

namespace sammine_lang::smn {

void SammineDialect::initialize() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "SammineTypes.cpp.inc"
      >();
  addOperations<
#define GET_OP_LIST
#include "SammineOps.cpp.inc"
      >();
}

} // namespace sammine_lang::smn
