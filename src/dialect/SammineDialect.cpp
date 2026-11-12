#include "dialect/SammineDialect.h"

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

bool ownsMemory(mlir::Type type) {
  if (auto ptr = mlir::dyn_cast<PtrType>(type))
    return ptr.isOwned();
  return false;
}

mlir::LogicalResult CopyOp::verify() {
  if (ownsMemory(getResult().getType()))
    return emitOpError("can't copy a value that owns memory; use "
                       "sammine.move");
  return mlir::success();
}

mlir::LogicalResult MoveOp::verify() {
  if (!ownsMemory(getResult().getType()))
    return emitOpError("moves only a value that owns memory; use "
                       "sammine.copy");
  return mlir::success();
}

} // namespace sammine_lang::smn
