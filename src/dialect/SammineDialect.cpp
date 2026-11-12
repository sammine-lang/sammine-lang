#include "dialect/SammineDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
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

mlir::LogicalResult
CallOp::verifySymbolUses(mlir::SymbolTableCollection &symbolTable) {
  auto callee = symbolTable.lookupNearestSymbolFrom<mlir::FunctionOpInterface>(
      *this, getCalleeAttr());
  if (!callee)
    return emitOpError("calls '")
           << getCallee() << "', which isn't a function";
  auto params = callee.getArgumentTypes();
  auto results = callee.getResultTypes();
  if (params.size() != getArgs().size() ||
      results.size() != getResults().size())
    return emitOpError("passes ")
           << getArgs().size() << " and takes back " << getResults().size()
           << ", but '" << getCallee() << "' takes " << params.size()
           << " and gives back " << results.size();
  // A callee emitted from the AST has LLVM types where MIR has its own.
  if (!callee->hasAttr(kMirAttr))
    return mlir::success();
  if (!llvm::equal(params, getArgs().getTypes()) ||
      !llvm::equal(results, getResults().getTypes()))
    return emitOpError("types don't match those of '")
           << getCallee() << "': " << callee.getFunctionType();
  return mlir::success();
}

} // namespace sammine_lang::smn
