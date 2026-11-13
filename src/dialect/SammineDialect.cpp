#include "dialect/SammineDialect.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/MathExtras.h"

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

/// The next multiple of `size` from `offset`: where a field of `size` goes.
static int64_t alignTo(int64_t offset, int64_t size) {
  return size == 0 ? offset
                   : static_cast<int64_t>(llvm::alignTo(
                         static_cast<uint64_t>(offset),
                         static_cast<uint64_t>(size)));
}

/// The bytes `fields` take laid out in order, then rounded up to the
/// largest of them.
static int64_t fieldsSize(llvm::ArrayRef<mlir::Type> fields) {
  int64_t size = payloadSize(fields), largest = 0;
  for (auto field : fields)
    largest = std::max(largest, sizeOf(field));
  return alignTo(size, largest);
}

int64_t sizeOf(mlir::Type type) {
  if (auto integer = mlir::dyn_cast<mlir::IntegerType>(type))
    return std::max<int64_t>(1, integer.getWidth() / 8);
  if (auto flt = mlir::dyn_cast<mlir::FloatType>(type))
    return flt.getWidth() / 8;
  if (mlir::isa<PtrType>(type))
    return 8;
  if (auto fn = mlir::dyn_cast<FnType>(type))
    return 8 + 8 * fn.getWords(); // its code, then the words it binds
  if (auto array = mlir::dyn_cast<ArrayType>(type))
    return sizeOf(array.getElement()) * array.getSize();
  if (auto st = mlir::dyn_cast<StructType>(type))
    return fieldsSize(st.getFields());
  if (auto tuple = mlir::dyn_cast<TupleType>(type))
    return fieldsSize(tuple.getElements());
  if (auto en = mlir::dyn_cast<EnumType>(type))
    return 4 + en.getPayloadSize(); // the i32 tag, then the payload
  return 0;
}

int64_t wordsFor(mlir::Type type) { return (sizeOf(type) + 7) / 8; }

llvm::SmallVector<int64_t> payloadOffsets(llvm::ArrayRef<mlir::Type> fields) {
  llvm::SmallVector<int64_t> offsets;
  int64_t offset = 0;
  for (auto field : fields) {
    offset = alignTo(offset, sizeOf(field));
    offsets.push_back(offset);
    offset += sizeOf(field);
  }
  return offsets;
}

int64_t payloadSize(llvm::ArrayRef<mlir::Type> fields) {
  if (fields.empty())
    return 0;
  return payloadOffsets(fields).back() + sizeOf(fields.back());
}

EnumType makeEnum(mlir::MLIRContext *ctx, llvm::StringRef name,
                  llvm::ArrayRef<mlir::StringAttr> variantNames,
                  llvm::ArrayRef<mlir::Type> variants) {
  int64_t payload = 0;
  for (auto variant : variants)
    payload = std::max(
        payload, payloadSize(mlir::cast<TupleType>(variant).getElements()));
  return EnumType::get(ctx, name, payload, variantNames, variants);
}

llvm::ArrayRef<mlir::Type> variantFields(EnumType en, uint64_t i) {
  return mlir::cast<TupleType>(en.getVariants()[i]).getElements();
}

void forEachBranchEdge(
    mlir::BranchOpInterface branch,
    llvm::function_ref<void(mlir::Value, mlir::BlockArgument)> fn) {
  for (unsigned i = 0; i < branch->getNumSuccessors(); ++i)
    for (auto [operand, arg] :
         llvm::zip(branch.getSuccessorOperands(i).getForwardedOperands(),
                   branch->getSuccessor(i)->getArguments()))
      fn(operand, arg);
}

bool definesMir(mlir::FunctionOpInterface func) {
  return func && !func.isExternal() && func->hasAttr(kMirAttr);
}

mlir::FunctionOpInterface dropFunction(mlir::ModuleOp module, mlir::Type type) {
  llvm::StringRef name;
  if (auto st = mlir::dyn_cast<StructType>(type))
    name = st.getName();
  else if (auto en = mlir::dyn_cast<EnumType>(type))
    name = en.getName();
  else
    return {};
  // A type holding function values of other lambda sets is named apart,
  // after a `#` (LambdaSets); its instance is its source type's.
  name = name.take_until([](char c) { return c == '#'; });
  return module.lookupSymbol<mlir::FunctionOpInterface>(
      ("Drop<" + name + ">::Drop").str());
}

std::optional<llvm::ArrayRef<mlir::Type>> fieldTypes(mlir::Type type) {
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

mlir::LogicalResult
EnumType::verify(llvm::function_ref<mlir::InFlightDiagnostic()> emitError,
                 llvm::StringRef enumName, int64_t,
                 llvm::ArrayRef<mlir::StringAttr> names,
                 llvm::ArrayRef<mlir::Type> variants) {
  if (names.size() != variants.size())
    return emitError() << "enum '" << enumName << "' names " << names.size()
                       << " variants but has " << variants.size();
  if (!llvm::all_of(variants, llvm::IsaPred<TupleType>))
    return emitError() << "enum '" << enumName
                       << "' has a payload that isn't a tuple";
  return mlir::success();
}

mlir::LogicalResult ThroughOp::verify() {
  auto ptr = mlir::dyn_cast<PtrType>(getPlace().getType().getValueType());
  if (!ptr)
    return emitOpError("goes through a place holding a pointer, not ")
           << getPlace().getType();
  if (getPointee().getType().getValueType() != ptr.getPointee())
    return emitOpError("gives the place the held pointer points at");
  return mlir::success();
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

mlir::LogicalResult DropOp::verify() {
  if (!ownsMemory(getValue().getType()))
    return emitOpError("drops only a value that owns memory, not ")
           << getValue().getType();
  return mlir::success();
}

mlir::LogicalResult LendOp::verify() {
  // A closure of a lambda set lowered to what it binds holds its captures
  // in an aggregate or a variant (LambdaSets).
  for (auto *user : getResult().getUsers())
    if (!mlir::isa<CallOp, ClosureOp>(user) && !user->hasAttr(kCapturesAttr))
      return emitOpError("is a view lent to a call or a closure, so only "
                         "sammine.call or sammine.closure can use it, not ")
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

bool ClosureOp::bindsInline() {
  auto bound = getBound();
  if (bound.empty())
    return true;
  auto carried =
      TupleType::get(getContext(), llvm::to_vector(bound.getTypes()));
  return wordsFor(carried) <= getType().getWords();
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
