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

// A struct's fields, `name: type`, comma-separated, as
// `custom<FieldList>`.
static mlir::ParseResult
parseFieldList(mlir::AsmParser &parser,
               llvm::SmallVector<mlir::StringAttr> &names,
               llvm::SmallVector<mlir::Type> &types) {
  auto parseField = [&]() -> mlir::ParseResult {
    std::string name;
    mlir::Type type;
    if (parser.parseKeywordOrString(&name) || parser.parseColon() ||
        parser.parseType(type))
      return mlir::failure();
    names.push_back(parser.getBuilder().getStringAttr(name));
    types.push_back(type);
    return mlir::success();
  };
  std::string first;
  if (mlir::failed(parser.parseOptionalKeywordOrString(&first)))
    return mlir::success();
  mlir::Type type;
  if (parser.parseColon() || parser.parseType(type))
    return mlir::failure();
  names.push_back(parser.getBuilder().getStringAttr(first));
  types.push_back(type);
  while (mlir::succeeded(parser.parseOptionalComma()))
    if (parseField())
      return mlir::failure();
  return mlir::success();
}

static void printFieldList(mlir::AsmPrinter &printer,
                           llvm::ArrayRef<mlir::StringAttr> names,
                           llvm::ArrayRef<mlir::Type> types) {
  llvm::interleaveComma(llvm::zip(names, types), printer, [&](auto field) {
    printer.printKeywordOrString(std::get<0>(field).getValue());
    printer << ": " << std::get<1>(field);
  });
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

/// An array's elements, as many as it holds.
static std::optional<llvm::SmallVector<mlir::Type>>
arrayElements(mlir::Type type) {
  auto array = mlir::dyn_cast<ArrayType>(type);
  if (!array)
    return std::nullopt;
  return llvm::SmallVector<mlir::Type>(static_cast<size_t>(array.getSize()),
                                       array.getElement());
}

bool ownsMemory(mlir::Type type) {
  if (auto ptr = mlir::dyn_cast<PtrType>(type))
    return ptr.isOwned();
  if (auto fields = fieldTypes(type))
    return llvm::any_of(*fields, ownsMemory);
  if (auto enumType = mlir::dyn_cast<EnumType>(type))
    return llvm::any_of(enumType.getVariants(), ownsMemory);
  if (auto array = mlir::dyn_cast<ArrayType>(type))
    return ownsMemory(array.getElement());
  return false;
}

mlir::LogicalResult
StructType::verify(llvm::function_ref<mlir::InFlightDiagnostic()> emitError,
                   llvm::StringRef structName,
                   llvm::ArrayRef<mlir::StringAttr> names,
                   llvm::ArrayRef<mlir::Type> fields) {
  if (names.size() != fields.size())
    return emitError() << "struct '" << structName << "' names " << names.size()
                       << " fields but has " << fields.size();
  return mlir::success();
}

std::string spelling(mlir::Type type) {
  auto list = [](llvm::ArrayRef<mlir::Type> types) {
    std::string s;
    for (auto [i, t] : llvm::enumerate(types))
      s += (i ? ", " : "") + spelling(t);
    return s;
  };
  if (auto ptr = mlir::dyn_cast<PtrType>(type))
    return (ptr.isOwned() ? "'ptr<" : "ptr<") + spelling(ptr.getPointee()) +
           ">";
  if (auto st = mlir::dyn_cast<StructType>(type))
    return st.getName().str();
  if (auto en = mlir::dyn_cast<EnumType>(type))
    return en.getName().str();
  if (auto tuple = mlir::dyn_cast<TupleType>(type))
    return "(" + list(tuple.getElements()) + ")";
  if (auto array = mlir::dyn_cast<ArrayType>(type))
    return "[" + std::to_string(array.getSize()) + "]" +
           spelling(array.getElement());
  if (auto fn = mlir::dyn_cast<FnType>(type)) {
    auto sig = fn.getSignature();
    return "(" + list(sig.getInputs()) + ") -> " +
           (sig.getNumResults() ? spelling(sig.getResult(0)) : "()");
  }
  if (type.isInteger(1))
    return "bool";
  if (type.isInteger(8))
    return "char";
  if (auto integer = mlir::dyn_cast<mlir::IntegerType>(type))
    return "i" + std::to_string(integer.getWidth());
  if (type.isF32())
    return "f32";
  if (type.isF64())
    return "f64";
  std::string s;
  llvm::raw_string_ostream os(s);
  type.print(os);
  return s;
}

mlir::LogicalResult FieldOp::verify() {
  auto base = getBase().getType().getValueType();
  auto fields = fieldTypes(base);
  if (!fields)
    return emitOpError("names a field of a struct or tuple, not ") << base;
  auto index = getIndex();
  if (index >= fields->size())
    return emitOpError("names field ")
           << index << " of " << base << ", which has " << fields->size();
  if (getPlace().getType().getValueType() != (*fields)[index])
    return emitOpError("gives a place of ")
           << getPlace().getType().getValueType() << " for field " << index
           << ", which is " << (*fields)[index];
  return mlir::success();
}

mlir::LogicalResult ElemOp::verify() {
  auto array = mlir::dyn_cast<ArrayType>(getBase().getType().getValueType());
  if (!array)
    return emitOpError("names an element of an array, not ")
           << getBase().getType().getValueType();
  if (getPlace().getType().getValueType() != array.getElement())
    return emitOpError("gives a place of ")
           << getPlace().getType().getValueType() << " for an element of "
           << array;
  return mlir::success();
}

mlir::LogicalResult ElementOp::verify() {
  if (getArray().getType().getElement() != getResult().getType())
    return emitOpError("gives an element of ")
           << getArray().getType() << " as " << getResult().getType();
  return mlir::success();
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
  if (auto elements = arrayElements(getResult().getType())) {
    if (!llvm::equal(*elements, getFields().getTypes()))
      return emitOpError("takes the elements of ") << getResult().getType();
    return mlir::success();
  }
  auto fields = fieldTypes(getResult().getType());
  if (!fields)
    return emitOpError("builds a struct, tuple or array, not ")
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

/// What a sammine call passes `callee` and gets back: its parameters, and
/// how many results. A C function returning a struct through memory takes
/// the buffer first (sret), a kernel's wrapper returning an array last
/// (kResultBufferAttr), and gives back nothing: the call gives back what it
/// fills.
static std::pair<llvm::ArrayRef<mlir::Type>, size_t>
sammineSignature(mlir::FunctionOpInterface callee) {
  auto params = callee.getArgumentTypes();
  size_t results = callee.getResultTypes().size();
  if (!params.empty() &&
      callee.getArgAttr(static_cast<unsigned>(params.size() - 1),
                        kResultBufferAttr))
    return {params.drop_back(), 1};
  if (auto cFunc = mlir::dyn_cast<mlir::LLVM::LLVMFuncOp>(*callee);
      cFunc && cFunc.getNumArguments() > 0 &&
      cFunc.getArgAttr(0, mlir::LLVM::LLVMDialect::getStructRetAttrName()))
    return {params.drop_front(), 1};
  return {params, results};
}

mlir::LogicalResult
ClosureOp::verifySymbolUses(mlir::SymbolTableCollection &symbolTable) {
  auto callee = symbolTable.lookupNearestSymbolFrom<mlir::FunctionOpInterface>(
      *this, getCalleeAttr());
  if (!callee)
    return emitOpError("binds '")
           << getCallee() << "', which isn't a function";
  auto [params, resultCount] = sammineSignature(callee);
  auto signature = getResult().getType().getSignature();
  if (getBound().size() + signature.getNumInputs() != params.size() ||
      signature.getNumResults() != resultCount)
    return emitOpError("binds ")
           << getBound().size() << " of '" << getCallee() << "', whose "
           << params.size() << " arguments don't fit " << signature;
  // A callee only declared, not MIR, has LLVM types where MIR has its own.
  if (!callee->hasAttr(kMirAttr))
    return mlir::success();
  if (!llvm::equal(params.take_front(getBound().size()),
                   getBound().getTypes()) ||
      !llvm::equal(params.drop_front(getBound().size()),
                   signature.getInputs()) ||
      !llvm::equal(callee.getResultTypes(), signature.getResults()))
    return emitOpError("types don't match those of '")
           << getCallee() << "': " << callee.getFunctionType();
  return mlir::success();
}

mlir::LogicalResult CallIndirectOp::verify() {
  auto signature = getCallee().getType().getSignature();
  if (!llvm::equal(signature.getInputs(), getArgs().getTypes()) ||
      !llvm::equal(signature.getResults(), getResults().getTypes()))
    return emitOpError("passes and takes back ")
           << mlir::FunctionType::get(getContext(), getArgs().getTypes(),
                                      getResults().getTypes())
           << ", but the function value is " << signature;
  return mlir::success();
}

mlir::LogicalResult
CallOp::verifySymbolUses(mlir::SymbolTableCollection &symbolTable) {
  auto callee = symbolTable.lookupNearestSymbolFrom<mlir::FunctionOpInterface>(
      *this, getCalleeAttr());
  if (!callee)
    return emitOpError("calls '")
           << getCallee() << "', which isn't a function";
  auto [params, resultCount] = sammineSignature(callee);
  auto results = callee.getResultTypes();
  // A C function's `...` takes any number more.
  auto cType =
      mlir::dyn_cast<mlir::LLVM::LLVMFunctionType>(callee.getFunctionType());
  bool variadic = cType && cType.isVarArg();
  if ((variadic ? getArgs().size() < params.size()
                : getArgs().size() != params.size()) ||
      resultCount != getResults().size())
    return emitOpError("passes ")
           << getArgs().size() << " and takes back " << getResults().size()
           << ", but '" << getCallee() << "' takes "
           << (variadic ? "at least " : "") << params.size()
           << " and gives back " << resultCount;
  // A callee only declared, not MIR, has LLVM types where MIR has its own.
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
