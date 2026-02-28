#include "codegen/MLIRGen.h"
#include "codegen/MLIRGenImpl.h"

#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include "fmt/core.h"

#include <algorithm>
#include <string>

namespace sammine_lang {

// ===--- generate() ---===

mlir::ModuleOp MLIRGenImpl::generate(AST::ProgramAST *program) {
  theModule = mlir::ModuleOp::create(builder.getUnknownLoc());

  declareRuntimeFunctions();

  // Register the closure struct type: { ptr, ptr }
  auto closurePtrTy = llvmPtrTy();
  closureType = mlir::LLVM::LLVMStructType::getIdentified(
      builder.getContext(), kClosureTypeName);
  (void)closureType.setBody({closurePtrTy, closurePtrTy}, /*isPacked=*/false);

  // Pre-pass: register struct types and forward-declare functions so that
  // references can find them even if the definition comes later.
  for (auto &def : program->DefinitionVec) {
    if (auto *sd = llvm::dyn_cast<AST::StructDefAST>(def.get())) {
      if (sd->type.type_kind != TypeKind::Poisoned) {
        auto &st = std::get<StructType>(sd->type.type_data);
        llvm::SmallVector<mlir::Type> fieldTypes;
        for (auto &ft : st.get_field_types())
          fieldTypes.push_back(convertType(ft));
        auto structTy = mlir::LLVM::LLVMStructType::getIdentified(
            builder.getContext(), kStructTypePrefix.str() + st.get_name());
        (void)structTy.setBody(fieldTypes, /*isPacked=*/false);
        structTypes[st.get_name()] = structTy;
      }
    } else if (auto *ed = llvm::dyn_cast<AST::EnumDefAST>(def.get())) {
      if (ed->type.type_kind != TypeKind::Poisoned) {
        auto &et = std::get<EnumType>(ed->type.type_data);
        // Integer-backed enums are bare integers — no struct registration
        if (et.is_integer_backed())
          continue;
        auto name = et.get_name().mangled();
        // Compute max payload size across all variants
        int64_t max_payload_size = 0;
        for (auto &vi : et.get_variants()) {
          int64_t variant_size = 0;
          for (auto &pt : vi.payload_types)
            variant_size += getTypeSize(pt);
          max_payload_size = std::max(max_payload_size, variant_size);
        }
        // Enum layout: { i32 tag, [N x i8] payload }
        llvm::SmallVector<mlir::Type> fields;
        fields.push_back(builder.getI32Type());
        if (max_payload_size > 0)
          fields.push_back(mlir::LLVM::LLVMArrayType::get(
              builder.getI8Type(), max_payload_size));
        auto enumTy = mlir::LLVM::LLVMStructType::getIdentified(
            builder.getContext(), "sammine.enum." + name);
        (void)enumTy.setBody(fields, /*isPacked=*/false);
        enumTypes[name] = enumTy;
      }
    } else if (auto *fd = llvm::dyn_cast<AST::FuncDefAST>(def.get())) {
      if (!fd->Prototype->is_generic())
        forwardDeclareFunc(fd->Prototype.get());
    } else if (auto *tci =
                   llvm::dyn_cast<AST::TypeClassInstanceAST>(def.get())) {
      for (auto &method : tci->methods)
        forwardDeclareFunc(method->Prototype.get());
    }
  }

  for (auto &def : program->DefinitionVec)
    emitDefinition(def.get());

  if (mlir::failed(mlir::verify(theModule))) {
    theModule.emitError("MLIR module verification failed");
    return nullptr;
  }

  return theModule;
}

// ===--- Location helpers ---===

mlir::Location MLIRGenImpl::loc(AST::AstBase *ast) {
  if (!ast)
    return builder.getUnknownLoc();

  auto srcLoc = ast->get_location();
  if (srcLoc.source_start <= 0 && srcLoc.source_end <= 0)
    return builder.getUnknownLoc();

  if (diagnosticData.empty())
    return builder.getUnknownLoc();

  // Binary search: find the line containing source_start
  auto cmp = [](const auto &a, const auto &b) { return a.first < b.first; };
  auto it = std::ranges::upper_bound(
      diagnosticData,
      std::make_pair(srcLoc.source_start, std::string_view("")), cmp);
  int64_t lineIdx =
      std::max(int64_t(it - diagnosticData.begin()), int64_t(1)) - 1;
  int64_t col = srcLoc.source_start - diagnosticData[lineIdx].first;

  // FileLineColLoc uses 1-based line and 0-based column
  return mlir::FileLineColLoc::get(builder.getContext(), fileName,
                                   static_cast<unsigned>(lineIdx + 1),
                                   static_cast<unsigned>(col));
}

// ===--- Runtime function declarations ---===

void MLIRGenImpl::declareRuntimeFunctions() {
  auto unknownLoc = builder.getUnknownLoc();
  auto ptrTy = llvmPtrTy();
  auto i32Ty = builder.getI32Type();
  auto i64Ty = builder.getI64Type();
  auto voidTy = llvmVoidTy();

  builder.setInsertionPointToEnd(theModule.getBody());

  // @malloc(i64) -> !llvm.ptr  [LLVM dialect — used by emitAllocExpr]
  {
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(ptrTy, {i64Ty});
    mlir::LLVM::LLVMFuncOp::create(builder, unknownLoc, kMallocFunc, fnTy);
  }
  // @free(!llvm.ptr) -> void  [LLVM dialect — used by emitFreeExpr]
  {
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(voidTy, {ptrTy});
    mlir::LLVM::LLVMFuncOp::create(builder, unknownLoc, kFreeFunc, fnTy);
  }
  // @exit(i32)  [func dialect — used by bounds check via func.call]
  {
    auto funcType = builder.getFunctionType({i32Ty}, {});
    auto funcOp = mlir::func::FuncOp::create(unknownLoc, kExitFunc, funcType);
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
    theModule.push_back(funcOp);
  }
}

// ===--- Type conversion ---===

mlir::Type MLIRGenImpl::convertType(const Type &type) {
  switch (type.type_kind) {
  case TypeKind::I32_t:
  case TypeKind::U32_t:
  case TypeKind::Integer:
    return builder.getI32Type();
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return builder.getI64Type();
  case TypeKind::F64_t:
  case TypeKind::Flt:
    return builder.getF64Type();
  case TypeKind::Bool:
    return builder.getI1Type();
  case TypeKind::Char:
    return builder.getI8Type();
  case TypeKind::Unit:
    return mlir::NoneType::get(builder.getContext());
  case TypeKind::String:
    // String is a pointer to i8 in LLVM
    return llvmPtrTy();
  case TypeKind::Pointer:
    return llvmPtrTy();
  case TypeKind::Function:
    // Function-typed values are represented as closure fat pointers
    return closureType;
  case TypeKind::Array: {
    auto &arrType = std::get<ArrayType>(type.type_data);
    auto elemMlirType = convertType(arrType.get_element());
    return mlir::LLVM::LLVMArrayType::get(elemMlirType, arrType.get_size());
  }
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(type.type_data);
    return structTypes.at(st.get_name());
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(type.type_data);
    if (et.is_integer_backed())
      return getEnumBackingMLIRType(et);
    return enumTypes.at(et.get_name().mangled());
  }
  case TypeKind::Tuple: {
    auto &tt = std::get<TupleType>(type.type_data);
    std::vector<mlir::Type> elemTypes;
    for (auto &et : tt.get_element_types())
      elemTypes.push_back(convertType(et));
    return mlir::LLVM::LLVMStructType::getLiteral(builder.getContext(),
                                                   elemTypes);
  }
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::TypeParam:
    sammine_util::abort(
        fmt::format("MLIRGen: unsupported type '{}'", type.to_string()));
  }
}

bool MLIRGenImpl::isIntegerType(const Type &type) {
  return type.type_kind == TypeKind::I32_t ||
         type.type_kind == TypeKind::I64_t ||
         type.type_kind == TypeKind::U32_t ||
         type.type_kind == TypeKind::U64_t ||
         type.type_kind == TypeKind::Integer ||
         type.type_kind == TypeKind::Char;
}

bool MLIRGenImpl::isUnsignedIntegerType(const Type &type) {
  return type.type_kind == TypeKind::U32_t ||
         type.type_kind == TypeKind::U64_t;
}

bool MLIRGenImpl::isFloatType(const Type &type) {
  return type.type_kind == TypeKind::F64_t || type.type_kind == TypeKind::Flt;
}

bool MLIRGenImpl::isBoolType(const Type &type) {
  return type.type_kind == TypeKind::Bool;
}

mlir::Type MLIRGenImpl::getEnumBackingMLIRType(const EnumType &et) {
  switch (et.get_backing_type()) {
  case TypeKind::I32_t:
  case TypeKind::U32_t:
    return builder.getI32Type();
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return builder.getI64Type();
  default:
    sammine_util::abort("Invalid enum backing type");
  }
}

// ===--- Definition emission ---===

void MLIRGenImpl::emitDefinition(AST::DefinitionAST *def) {
  if (auto *funcDef = llvm::dyn_cast<AST::FuncDefAST>(def))
    emitFunction(funcDef);
  else if (auto *externDef = llvm::dyn_cast<AST::ExternAST>(def))
    emitExtern(externDef);
  else if (llvm::isa<AST::StructDefAST>(def))
    ; // Struct types registered in generate() pre-pass
  else if (llvm::isa<AST::EnumDefAST>(def))
    ; // Enum types registered in generate() pre-pass
  else if (llvm::isa<AST::TypeAliasDefAST>(def))
    ; // Type aliases resolved at type-check time
  else if (llvm::isa<AST::TypeClassDeclAST>(def))
    ; // Skip typeclass declarations (no codegen needed)
  else if (auto *tci = llvm::dyn_cast<AST::TypeClassInstanceAST>(def)) {
    // Emit each instance method as a regular function
    for (auto &method : tci->methods)
      emitFunction(method.get());
  } else {
    sammine_util::abort(fmt::format(
        "MLIRGen: unknown definition type '{}'", def->getTreeName()));
  }
}

mlir::FunctionType MLIRGenImpl::buildFuncType(AST::PrototypeAST *proto) {
  llvm::SmallVector<mlir::Type, 4> argTypes;
  for (auto &param : proto->parameterVectors)
    argTypes.push_back(convertType(param->type));

  llvm::SmallVector<mlir::Type, 1> retTypes;
  // proto->type is the full FunctionType — extract the return type from it
  if (proto->type.type_kind == TypeKind::Function) {
    auto &funcType = std::get<FunctionType>(proto->type.type_data);
    auto retType = funcType.get_return_type();
    if (retType.type_kind == TypeKind::Array) {
      // sret: array returns become a trailing !llvm.ptr parameter, void return
      argTypes.push_back(llvmPtrTy());
    } else if (retType.type_kind != TypeKind::Unit) {
      retTypes.push_back(convertType(retType));
    }
  }

  return builder.getFunctionType(argTypes, retTypes);
}

std::string
MLIRGenImpl::mangleName(const sammine_util::QualifiedName &qn) const {
  return qn.with_module(moduleName).mangled();
}

// ===--- Expression dispatcher ---===

mlir::Value MLIRGenImpl::emitExpr(AST::ExprAST *ast) {
  if (auto *num = llvm::dyn_cast<AST::NumberExprAST>(ast))
    return emitNumberExpr(num);
  if (auto *boolE = llvm::dyn_cast<AST::BoolExprAST>(ast))
    return emitBoolExpr(boolE);
  if (auto *charE = llvm::dyn_cast<AST::CharExprAST>(ast))
    return emitCharExpr(charE);
  if (auto *unit = llvm::dyn_cast<AST::UnitExprAST>(ast))
    return emitUnitExpr(unit);
  if (auto *var = llvm::dyn_cast<AST::VariableExprAST>(ast))
    return emitVariableExpr(var);
  if (auto *bin = llvm::dyn_cast<AST::BinaryExprAST>(ast))
    return emitBinaryExpr(bin);
  if (auto *call = llvm::dyn_cast<AST::CallExprAST>(ast))
    return emitCallExpr(call);
  if (auto *ret = llvm::dyn_cast<AST::ReturnExprAST>(ast))
    return emitReturnExpr(ret);
  if (auto *varDef = llvm::dyn_cast<AST::VarDefAST>(ast))
    return emitVarDef(varDef);
  if (auto *ifE = llvm::dyn_cast<AST::IfExprAST>(ast))
    return emitIfExpr(ifE);
  if (auto *whileE = llvm::dyn_cast<AST::WhileExprAST>(ast))
    return emitWhileExpr(whileE);
  if (auto *neg = llvm::dyn_cast<AST::UnaryNegExprAST>(ast))
    return emitUnaryNegExpr(neg);
  if (auto *str = llvm::dyn_cast<AST::StringExprAST>(ast))
    return emitStringExpr(str);
  if (auto *arrLit = llvm::dyn_cast<AST::ArrayLiteralExprAST>(ast))
    return emitArrayLiteralExpr(arrLit);
  if (auto *idx = llvm::dyn_cast<AST::IndexExprAST>(ast))
    return emitIndexExpr(idx);
  if (auto *len = llvm::dyn_cast<AST::LenExprAST>(ast))
    return emitLenExpr(len);
  if (auto *deref = llvm::dyn_cast<AST::DerefExprAST>(ast))
    return emitDerefExpr(deref);
  if (auto *addrOf = llvm::dyn_cast<AST::AddrOfExprAST>(ast))
    return emitAddrOfExpr(addrOf);
  if (auto *alloc = llvm::dyn_cast<AST::AllocExprAST>(ast))
    return emitAllocExpr(alloc);
  if (auto *freeE = llvm::dyn_cast<AST::FreeExprAST>(ast))
    return emitFreeExpr(freeE);
  if (auto *structLit = llvm::dyn_cast<AST::StructLiteralExprAST>(ast))
    return emitStructLiteralExpr(structLit);
  if (auto *fieldAccess = llvm::dyn_cast<AST::FieldAccessExprAST>(ast))
    return emitFieldAccessExpr(fieldAccess);
  if (auto *caseExpr = llvm::dyn_cast<AST::CaseExprAST>(ast))
    return emitCaseExpr(caseExpr);
  if (auto *tupleLit = llvm::dyn_cast<AST::TupleLiteralExprAST>(ast))
    return emitTupleLiteralExpr(tupleLit);

  sammine_util::abort(
      fmt::format("MLIRGen: unsupported expression type '{}'",
                  ast->getTreeName()));
}

// ===--- Block and variable definition ---===

mlir::Value MLIRGenImpl::emitBlock(AST::BlockAST *ast) {
  mlir::Value lastVal = nullptr;

  for (auto &stmt : ast->Statements) {
    // If the current block already has a terminator (e.g. a return),
    // don't emit more statements after it.
    auto *currentBlock = builder.getInsertionBlock();
    if (!currentBlock->empty() &&
        currentBlock->back().hasTrait<mlir::OpTrait::IsTerminator>())
      break;

    lastVal = emitExpr(stmt.get());
  }

  return lastVal;
}

mlir::Value MLIRGenImpl::emitVarDef(AST::VarDefAST *ast) {
  auto location = loc(ast);
  mlir::Value initVal = emitExpr(ast->Expression.get());
  if (!initVal)
    return nullptr;

  // Tuple destructuring: let (a, b) = expr;
  if (ast->is_tuple_destructure) {
    for (size_t i = 0; i < ast->destructure_vars.size(); i++) {
      auto elemVal = mlir::LLVM::ExtractValueOp::create(
          builder, location, initVal,
          llvm::ArrayRef<int64_t>{static_cast<int64_t>(i)});
      auto elemType = convertType(ast->destructure_vars[i]->type);
      auto alloca = emitAllocaOne(elemType, location);
      mlir::LLVM::StoreOp::create(builder, location, elemVal, alloca);
      symbolTable.registerNameT(ast->destructure_vars[i]->name, alloca);
    }
    return initVal;
  }

  // Arrays: the literal already returns an !llvm.ptr, register it directly
  if (ast->type.type_kind == TypeKind::Array) {
    symbolTable.registerNameT(ast->TypedVar->name, initVal);
    return initVal;
  }

  // All non-array variables: llvm.alloca + llvm.store (uniform model)
  auto alloca = emitAllocaOne(convertType(ast->type), location);
  mlir::LLVM::StoreOp::create(builder, location, initVal, alloca);
  symbolTable.registerNameT(ast->TypedVar->name, alloca);
  return initVal;
}

// ===--- Helpers ---===

mlir::Value MLIRGenImpl::emitAllocaOne(mlir::Type elemType,
                                        mlir::Location location) {
  auto one = mlir::arith::ConstantIntOp::create(builder, location,
                                                  builder.getI64Type(), 1);
  return mlir::LLVM::AllocaOp::create(builder, location, llvmPtrTy(),
                                        elemType, one);
}

int64_t MLIRGenImpl::getTypeSize(const Type &type) {
  switch (type.type_kind) {
  case TypeKind::I32_t:
  case TypeKind::U32_t:
  case TypeKind::Integer:
    return 4;
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return 8;
  case TypeKind::F64_t:
  case TypeKind::Flt:
    return 8;
  case TypeKind::Bool:
  case TypeKind::Char:
    return 1;
  case TypeKind::Pointer:
  case TypeKind::String:
    return 8;
  case TypeKind::Function:
    return 16; // closure struct: two pointers
  case TypeKind::Array: {
    auto &arr = std::get<ArrayType>(type.type_data);
    return getTypeSize(arr.get_element()) * static_cast<int64_t>(arr.get_size());
  }
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(type.type_data);
    int64_t size = 0;
    for (auto &ft : st.get_field_types()) {
      int64_t fieldSize = getTypeSize(ft);
      int64_t align = fieldSize; // natural alignment
      size = llvm::alignTo(size, align);
      size += fieldSize;
    }
    // Align total to largest field alignment
    if (!st.get_field_types().empty()) {
      int64_t maxAlign = 0;
      for (auto &ft : st.get_field_types())
        maxAlign = std::max(maxAlign, getTypeSize(ft));
      size = llvm::alignTo(size, maxAlign);
    }
    return size;
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(type.type_data);
    if (et.is_integer_backed()) {
      auto bt = et.get_backing_type();
      return (bt == TypeKind::I64_t || bt == TypeKind::U64_t) ? 8 : 4;
    }
    int64_t max_payload = 0;
    for (auto &vi : et.get_variants()) {
      int64_t variant_size = 0;
      for (auto &pt : vi.payload_types)
        variant_size += getTypeSize(pt);
      max_payload = std::max(max_payload, variant_size);
    }
    return 4 + max_payload; // i32 tag + payload
  }
  case TypeKind::Tuple: {
    auto &tt = std::get<TupleType>(type.type_data);
    int64_t size = 0;
    for (auto &et : tt.get_element_types()) {
      int64_t fieldSize = getTypeSize(et);
      int64_t align = fieldSize;
      size = llvm::alignTo(size, align);
      size += fieldSize;
    }
    if (!tt.get_element_types().empty()) {
      int64_t maxAlign = 0;
      for (auto &et : tt.get_element_types())
        maxAlign = std::max(maxAlign, getTypeSize(et));
      size = llvm::alignTo(size, maxAlign);
    }
    return size;
  }
  default:
    sammine_util::abort(
        fmt::format("MLIRGen: cannot compute size of type '{}'",
                    type.to_string()));
  }
}

mlir::Value MLIRGenImpl::getOrCreateGlobalString(llvm::StringRef name,
                                                  llvm::StringRef value,
                                                  mlir::Location location) {
  // Check if we already have this global
  auto globalOp = theModule.lookupSymbol<mlir::LLVM::GlobalOp>(name);
  if (!globalOp) {
    // Add null terminator for C interop
    std::string nullTerminated = value.str();
    nullTerminated.push_back('\0');

    mlir::OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(theModule.getBody());

    auto strType = mlir::LLVM::LLVMArrayType::get(
        mlir::IntegerType::get(builder.getContext(), 8),
        nullTerminated.size());
    globalOp = mlir::LLVM::GlobalOp::create(
        builder, location, strType, /*isConstant=*/true,
        mlir::LLVM::Linkage::Internal, name,
        builder.getStringAttr(nullTerminated));
  }

  // Get address of the global
  auto ptrTy = llvmPtrTy();
  return mlir::LLVM::AddressOfOp::create(builder, location, ptrTy, name);
}

// ===--- Public entry point ---===

mlir::OwningOpRef<mlir::ModuleOp>
mlirGen(mlir::MLIRContext &context, AST::ProgramAST *program,
        const std::string &moduleName, const std::string &fileName,
        const std::string &sourceText) {
  return MLIRGenImpl(context, moduleName, fileName, sourceText)
      .generate(program);
}

} // namespace sammine_lang
