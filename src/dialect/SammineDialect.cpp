#include "dialect/SammineDialect.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "llvm/ADT/TypeSwitch.h"

#include "SammineDialect.cpp.inc"

#include "SammineEnums.cpp.inc"

// A comma-separated list of types, which may be empty: a tuple's elements,
// a struct's fields, an enum's variants' payloads, as `custom<TypeList>`.
static mlir::ParseResult
parseTypeList(mlir::AsmParser &parser, llvm::SmallVector<mlir::Type> &types) {
  mlir::Type type;
  auto first = parser.parseOptionalType(type);
  if (!first.has_value())
    return mlir::success();
  if (mlir::failed(*first))
    return mlir::failure();
  types.push_back(type);
  while (mlir::succeeded(parser.parseOptionalComma())) {
    if (parser.parseType(type))
      return mlir::failure();
    types.push_back(type);
  }
  return mlir::success();
}

static void printTypeList(mlir::AsmPrinter &printer,
                          llvm::ArrayRef<mlir::Type> types) {
  llvm::interleaveComma(types, printer);
}

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

/// The types of the fields of a struct or tuple, or nullopt for any other
/// type.
static std::optional<llvm::ArrayRef<mlir::Type>> fieldTypes(mlir::Type type) {
  if (auto st = mlir::dyn_cast<StructType>(type))
    return st.getFields();
  if (auto tuple = mlir::dyn_cast<TupleType>(type))
    return tuple.getElements();
  return std::nullopt;
}

bool ownsMemory(mlir::Type type) {
  if (auto ptr = mlir::dyn_cast<PtrType>(type))
    return ptr.isOwned();
  if (auto fields = fieldTypes(type))
    return llvm::any_of(*fields, ownsMemory);
  if (auto enumType = mlir::dyn_cast<EnumType>(type))
    return llvm::any_of(enumType.getVariants(), ownsMemory);
  return false;
}

mlir::LogicalResult VariantOp::verify() {
  auto type = getResult().getType();
  auto index = getIndex();
  if (index >= type.getVariants().size())
    return emitOpError("builds variant ")
           << index << " of " << type << ", which has "
           << type.getVariants().size();
  if (!llvm::equal(type.getPayload(index), getPayload().getTypes()))
    return emitOpError("takes the payload of variant ")
           << index << " of " << type;
  if (getOffsets().size() != getPayload().size())
    return emitOpError("needs an offset for each payload field");
  return mlir::success();
}

mlir::LogicalResult PayloadOp::verify() {
  auto type = getValue().getType();
  auto variant = getVariant();
  if (variant >= type.getVariants().size())
    return emitOpError("reads variant ")
           << variant << " of " << type << ", which has "
           << type.getVariants().size();
  auto payload = type.getPayload(variant);
  auto field = getField();
  if (field >= payload.size())
    return emitOpError("reads field ")
           << field << " of variant " << variant << "'s payload, which has "
           << payload.size();
  if (payload[field] != getResult().getType())
    return emitOpError("gives the field as ")
           << getResult().getType() << ", but it is " << payload[field];
  return mlir::success();
}

mlir::LogicalResult AggregateOp::verify() {
  auto fields = fieldTypes(getResult().getType());
  if (!fields)
    return emitOpError("builds a struct or tuple, not ")
           << getResult().getType();
  if (!llvm::equal(*fields, getFields().getTypes()))
    return emitOpError("takes the types of the fields of ")
           << getResult().getType();
  return mlir::success();
}

mlir::LogicalResult ExtractOp::verify() {
  auto fields = fieldTypes(getAggregate().getType());
  if (!fields)
    return emitOpError("reads a field of a struct or tuple, not ")
           << getAggregate().getType();
  auto index = getIndex();
  if (index >= fields->size())
    return emitOpError("reads field ")
           << index << " of " << getAggregate().getType() << ", which has "
           << fields->size();
  if ((*fields)[index] != getResult().getType())
    return emitOpError("gives field ")
           << index << " as " << getResult().getType() << ", but it is "
           << (*fields)[index];
  return mlir::success();
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

mlir::LogicalResult LendOp::verify() {
  for (auto *user : getResult().getUsers())
    if (!mlir::isa<CallOp>(user))
      return emitOpError("is a view for the length of a call, so only "
                         "sammine.call can use it, not ")
             << user->getName();
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
  // A C function's `...` takes any number more.
  auto cType =
      mlir::dyn_cast<mlir::LLVM::LLVMFunctionType>(callee.getFunctionType());
  bool variadic = cType && cType.isVarArg();
  if ((variadic ? getArgs().size() < params.size()
                : getArgs().size() != params.size()) ||
      results.size() != getResults().size())
    return emitOpError("passes ")
           << getArgs().size() << " and takes back " << getResults().size()
           << ", but '" << getCallee() << "' takes "
           << (variadic ? "at least " : "") << params.size()
           << " and gives back " << results.size();
  // A callee emitted from the AST has LLVM types where MIR has its own.
  if (!callee->hasAttr(kMirAttr))
    return mlir::success();
  if (!llvm::equal(params, getArgs().getTypes()) ||
      !llvm::equal(results, getResults().getTypes()))
    return emitOpError("types don't match those of '")
           << getCallee() << "': " << callee.getFunctionType() << ", it passes "
           << mlir::FunctionType::get(getContext(), getArgs().getTypes(),
                                      getResults().getTypes());
  return mlir::success();
}

} // namespace sammine_lang::smn
