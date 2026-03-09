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
// Entry point for MLIR code generation. Uses direct recursive dispatch
// (emitExpr → dynamic_cast chain) rather than the visitor pattern, because
// codegen needs to return mlir::Value from each node.
//
// Three pre-passes before emitting definitions:
// 1) Declare runtime functions (malloc, free, exit)
// 2) Register all struct/enum LLVM types (so forward refs work)
// 3) Forward-declare all function signatures (needed for function-as-value refs)

mlir::ModuleOp MLIRGenImpl::generate(AST::ProgramAST *program) {
  theModule = mlir::ModuleOp::create(builder.getUnknownLoc());

  declareRuntimeFunctions();

  // Register the closure struct type: { ptr, ptr }
  auto closurePtrTy = llvmPtrTy();
  closureType = mlir::LLVM::LLVMStructType::getIdentified(
      builder.getContext(), kClosureTypeName);
  (void)closureType.setBody({closurePtrTy, closurePtrTy}, /*isPacked=*/false);

  // Pre-pass 1: register struct and enum types so that function
  // forward-declarations can reference them (monomorphized functions may
  // appear before imported struct definitions in the DefinitionVec).
  for (auto &def : program->DefinitionVec) {
    if (auto *sd = llvm::dyn_cast<AST::StructDefAST>(def.get())) {
      if (sd->get_type().type_kind == TypeKind::Struct) {
        auto st = std::get<StructType>(sd->get_type().type_data);
        llvm::SmallVector<mlir::Type> fieldTypes;
        for (auto &ft : st.get_field_types())
          fieldTypes.push_back(convertType(ft));
        auto structTy = mlir::LLVM::LLVMStructType::getIdentified(
            builder.getContext(),
            kStructTypePrefix.str() + st.get_name().mangled());
        (void)structTy.setBody(fieldTypes, /*isPacked=*/false);
        structTypes[st.get_name().mangled()] = structTy;
      }
    } else if (auto *ed = llvm::dyn_cast<AST::EnumDefAST>(def.get())) {
      if (ed->get_type().type_kind == TypeKind::Enum) {
        auto et = std::get<EnumType>(ed->get_type().type_data);
        // Integer-backed enums are bare integers — no struct registration
        if (et.is_integer_backed())
          continue;
        auto name = et.get_name().mangled();
        // Compute max payload size across all variants
        int64_t max_payload_size = 0;
        for (auto &vi : et.get_variants())
          max_payload_size = std::max(max_payload_size,
                                       getVariantPayloadSize(vi.payload_types));
        // Enum layout: { i32 tag, [N x i8] payload }
        llvm::SmallVector<mlir::Type> fields;
        fields.push_back(builder.getI32Type());
        if (max_payload_size > 0)
          fields.push_back(mlir::LLVM::LLVMArrayType::get(
              builder.getI8Type(), static_cast<uint64_t>(max_payload_size)));
        auto enumTy = mlir::LLVM::LLVMStructType::getIdentified(
            builder.getContext(), "sammine.enum." + name);
        (void)enumTy.setBody(fields, /*isPacked=*/false);
        enumTypes[name] = enumTy;
      }
    }
  }

  // Pre-pass 2: forward-declare functions (now that all types are registered).
  for (auto &def : program->DefinitionVec) {
    if (auto *fd = llvm::dyn_cast<AST::FuncDefAST>(def.get())) {
      if (!fd->Prototype->is_generic())
        forwardDeclareFunc(fd->Prototype.get());
    } else if (auto *tci =
                   llvm::dyn_cast<AST::TypeClassInstanceAST>(def.get())) {
      for (auto &method : tci->methods)
        forwardDeclareFunc(method->Prototype.get());
    } else if (auto *kd = llvm::dyn_cast<AST::KernelDefAST>(def.get())) {
      if (!kd->Prototype->is_generic())
        forwardDeclareFunc(kd->Prototype.get());
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
  int64_t col = srcLoc.source_start - diagnosticData[static_cast<size_t>(lineIdx)].first;

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
// Maps sammine Types to MLIR types. Scalars → builtin integer/float types.
// Pointers → !llvm.ptr (opaque). Arrays → LLVMArrayType. Structs/enums → named
// LLVMStructType. Functions → closure fat pointer struct { code_ptr, env_ptr }.

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
  case TypeKind::F32_t:
    return builder.getF32Type();
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
    return structTypes.at(st.get_name().mangled());
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
  case TypeKind::NonExistent:
    imm_error("type was never resolved (NonExistent) — "
              "this is a compiler bug: a type expression was not "
              "visited during type checking");
  case TypeKind::Poisoned:
    imm_error("poisoned type reached codegen — "
              "a type error should have been reported and "
              "compilation halted before codegen");
  case TypeKind::Generic:
    imm_error("generic template type reached codegen — "
              "this is a compiler bug: the generic definition was "
              "not monomorphized before codegen");
  case TypeKind::TypeParam:
    imm_error(fmt::format(
        "unresolved type parameter '{}' reached codegen — "
        "this is a compiler bug: the type parameter was not substituted "
        "during monomorphization",
        type.to_string()));
  case TypeKind::Never:
    imm_error("'never' type reached codegen — "
              "this is a compiler bug: a diverging expression's "
              "type was not handled before codegen");
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
  return type.type_kind == TypeKind::F64_t || type.type_kind == TypeKind::F32_t ||
         type.type_kind == TypeKind::Flt;
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
    imm_error("invalid enum backing type — expected i32, i64, u32, or u64");
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
  } else if (auto *kd = llvm::dyn_cast<AST::KernelDefAST>(def)) {
    emitKernelDef(kd);
  } else {
    imm_error(fmt::format("unknown definition type '{}'", def->getTreeName()),
              def->get_location());
  }
}

void MLIRGenImpl::emitKernelDef(AST::KernelDefAST *kd) {
  auto funcName = mangleName(kd->Prototype->functionName);
  auto funcType = buildFuncType(kd->Prototype.get());
  auto location = loc(kd);

  auto funcOp = theModule.lookupSymbol<mlir::func::FuncOp>(funcName);

  auto &entryBlock = *funcOp.addEntryBlock();
  builder.setInsertionPointToStart(&entryBlock);

  // TODO: ad-hoc returning — currently hardcodes return of the first expression.
  // Should properly handle multiple expressions and explicit return statements.
  if (!kd->Body->expressions.empty()) {
    if (auto *numExpr = llvm::dyn_cast<AST::KernelNumberExprAST>(
            kd->Body->expressions[0].get())) {
      auto retType = funcType.getResults()[0];
      int64_t val = std::stoll(numExpr->number);
      auto constVal =
          mlir::arith::ConstantIntOp::create(builder, location, retType, val);
      mlir::func::ReturnOp::create(builder, location,
                                   mlir::ValueRange{constVal});
    } else if (llvm::isa<AST::KernelMapExprAST>(
                   kd->Body->expressions[0].get()) ||
               llvm::isa<AST::KernelReduceExprAST>(
                   kd->Body->expressions[0].get())) {
      // TODO: implement proper map/reduce lowering
      if (funcType.getResults().empty()) {
        mlir::func::ReturnOp::create(builder, location, mlir::ValueRange{});
      } else {
        auto retType = funcType.getResults()[0];
        mlir::Value zero;
        if (mlir::isa<mlir::FloatType>(retType)) {
          zero = mlir::arith::ConstantFloatOp::create(
              builder, location, mlir::cast<mlir::FloatType>(retType), llvm::APFloat(0.0));
        } else {
          zero = mlir::arith::ConstantIntOp::create(builder, location, retType, 0);
        }
        mlir::func::ReturnOp::create(builder, location,
                                     mlir::ValueRange{zero});
      }
    }
  }
}

mlir::FunctionType MLIRGenImpl::buildFuncType(AST::PrototypeAST *proto) {
  llvm::SmallVector<mlir::Type, 4> argTypes;
  for (auto &param : proto->parameterVectors)
    argTypes.push_back(convertType(param->get_type()));

  llvm::SmallVector<mlir::Type, 1> retTypes;
  // proto->get_type() is the full FunctionType — extract the return type from it
  if (proto->get_type().type_kind == TypeKind::Function) {
    auto funcType = std::get<FunctionType>(proto->get_type().type_data);
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
// Central dispatch for all expressions. Uses dynamic_cast chain (not visitor)
// because each emit* method returns an mlir::Value.

mlir::Value MLIRGenImpl::emitExpr(AST::ExprAST *ast) {
  using NK = AST::NodeKind;
  switch (ast->getKind()) {
  case NK::NumberExprAST:
    return emitNumberExpr(llvm::cast<AST::NumberExprAST>(ast));
  case NK::BoolExprAST:
    return emitBoolExpr(llvm::cast<AST::BoolExprAST>(ast));
  case NK::CharExprAST:
    return emitCharExpr(llvm::cast<AST::CharExprAST>(ast));
  case NK::UnitExprAST:
    return emitUnitExpr(llvm::cast<AST::UnitExprAST>(ast));
  case NK::VariableExprAST:
    return emitVariableExpr(llvm::cast<AST::VariableExprAST>(ast));
  case NK::BinaryExprAST:
    return emitBinaryExpr(llvm::cast<AST::BinaryExprAST>(ast));
  case NK::CallExprAST:
    return emitCallExpr(llvm::cast<AST::CallExprAST>(ast));
  case NK::ReturnExprAST:
    return emitReturnExpr(llvm::cast<AST::ReturnExprAST>(ast));
  case NK::VarDefAST:
    return emitVarDef(llvm::cast<AST::VarDefAST>(ast));
  case NK::IfExprAST:
    return emitIfExpr(llvm::cast<AST::IfExprAST>(ast));
  case NK::WhileExprAST:
    return emitWhileExpr(llvm::cast<AST::WhileExprAST>(ast));
  case NK::UnaryNegExprAST:
    return emitUnaryNegExpr(llvm::cast<AST::UnaryNegExprAST>(ast));
  case NK::StringExprAST:
    return emitStringExpr(llvm::cast<AST::StringExprAST>(ast));
  case NK::ArrayLiteralExprAST:
    return emitArrayLiteralExpr(llvm::cast<AST::ArrayLiteralExprAST>(ast));
  case NK::IndexExprAST:
    return emitIndexExpr(llvm::cast<AST::IndexExprAST>(ast));
  case NK::LenExprAST:
    return emitLenExpr(llvm::cast<AST::LenExprAST>(ast));
  case NK::DimExprAST:
    return emitDimExpr(llvm::cast<AST::DimExprAST>(ast));
  case NK::DerefExprAST:
    return emitDerefExpr(llvm::cast<AST::DerefExprAST>(ast));
  case NK::AddrOfExprAST:
    return emitAddrOfExpr(llvm::cast<AST::AddrOfExprAST>(ast));
  case NK::AllocExprAST:
    return emitAllocExpr(llvm::cast<AST::AllocExprAST>(ast));
  case NK::FreeExprAST:
    return emitFreeExpr(llvm::cast<AST::FreeExprAST>(ast));
  case NK::StructLiteralExprAST:
    return emitStructLiteralExpr(llvm::cast<AST::StructLiteralExprAST>(ast));
  case NK::FieldAccessExprAST:
    return emitFieldAccessExpr(llvm::cast<AST::FieldAccessExprAST>(ast));
  case NK::CaseExprAST:
    return emitCaseExpr(llvm::cast<AST::CaseExprAST>(ast));
  case NK::TupleLiteralExprAST:
    return emitTupleLiteralExpr(llvm::cast<AST::TupleLiteralExprAST>(ast));
  default:
    imm_error(fmt::format("unsupported expression type '{}'",
                          ast->getTreeName()),
              ast->get_location());
  }
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
      auto elemType = convertType(ast->destructure_vars[i]->get_type());
      auto alloca = emitAllocaOne(elemType, location);
      mlir::LLVM::StoreOp::create(builder, location, elemVal, alloca);
      symbolTable.registerNameT(ast->destructure_vars[i]->name, alloca);
    }
    return initVal;
  }

  // Arrays: initVal is an !llvm.ptr to the source array data.
  if (ast->get_type().type_kind == TypeKind::Array) {
    // For immutable arrays with all-constant elements, emit as global constant
    if (!ast->is_mutable) {
      if (auto *arrLit = llvm::dyn_cast<AST::ArrayLiteralExprAST>(
              ast->Expression.get())) {
        bool allConst = std::all_of(
            arrLit->elements.begin(), arrLit->elements.end(),
            [](const auto &e) {
              return llvm::isa<AST::NumberExprAST>(e.get()) ||
                     llvm::isa<AST::BoolExprAST>(e.get()) ||
                     llvm::isa<AST::CharExprAST>(e.get());
            });
        if (allConst) {
          auto globalPtr =
              emitGlobalConstArray(arrLit, ast->get_type(), location);
          symbolTable.registerNameT(ast->TypedVar->name, globalPtr);
          return globalPtr;
        }
      }
    }

    // Mutable arrays need their own copy (alloca + load/store) so writes
    // don't alias the source (which may be a global constant or another var).
    if (ast->is_mutable) {
      auto llvmArrayType = mlir::cast<mlir::LLVM::LLVMArrayType>(
          convertType(ast->get_type()));
      auto alloca = emitAllocaOne(llvmArrayType, location);
      auto loaded =
          mlir::LLVM::LoadOp::create(builder, location, llvmArrayType, initVal);
      mlir::LLVM::StoreOp::create(builder, location, loaded, alloca);
      symbolTable.registerNameT(ast->TypedVar->name, alloca);
      return alloca;
    }

    symbolTable.registerNameT(ast->TypedVar->name, initVal);
    return initVal;
  }

  // All non-array variables: llvm.alloca + llvm.store (uniform model)
  auto alloca = emitAllocaOne(convertType(ast->get_type()), location);
  mlir::LLVM::StoreOp::create(builder, location, initVal, alloca);
  symbolTable.registerNameT(ast->TypedVar->name, alloca);
  return initVal;
}

// ===--- Helpers ---===

mlir::Value MLIRGenImpl::emitAllocaOne(mlir::Type elemType,
                                        mlir::Location location) {
  // Hoist alloca to the entry block so Mem2Reg/SROA can eliminate it.
  mlir::OpBuilder::InsertionGuard guard(builder);
  auto *currentBlock = builder.getInsertionBlock();
  auto &entryBlock = currentBlock->getParent()->front();
  builder.setInsertionPointToStart(&entryBlock);

  auto one = mlir::arith::ConstantIntOp::create(builder, location,
                                                  builder.getI64Type(), 1);
  return mlir::LLVM::AllocaOp::create(builder, location, llvmPtrTy(),
                                        elemType, one);
}

mlir::Value MLIRGenImpl::emitRValue(AST::ExprAST *ast) {
  auto val = emitExpr(ast);
  if (!val)
    return nullptr;
  auto semTy = convertType(ast->get_type());
  if (mlir::isa<mlir::LLVM::LLVMPointerType>(val.getType()) &&
      !mlir::isa<mlir::LLVM::LLVMPointerType>(semTy))
    return mlir::LLVM::LoadOp::create(builder, loc(ast), semTy, val);
  return val;
}

mlir::Value MLIRGenImpl::emitLValue(AST::ExprAST *ast) {
  auto location = loc(ast);

  // Variable: return its alloca
  if (auto *var = llvm::dyn_cast<AST::VariableExprAST>(ast))
    return symbolTable.get_from_name(var->variableName);

  // Dereference: lvalue of *p is the value of p
  if (auto *deref = llvm::dyn_cast<AST::DerefExprAST>(ast))
    return emitRValue(deref->operand.get());

  // Index: return GEP pointer to element
  if (auto *idx = llvm::dyn_cast<AST::IndexExprAST>(ast)) {
    auto arr = emitExpr(idx->array_expr.get());
    auto idxVal = emitExpr(idx->index_expr.get());
    if (!arr || !idxVal)
      return nullptr;

    auto baseType = idx->array_expr->get_type();

    if (baseType.type_kind == TypeKind::Pointer) {
      auto &ptrData = std::get<PointerType>(baseType.type_data);
      return emitPtrElementGEP(arr, idxVal, ptrData.get_pointee(), location);
    }

    auto arrType = std::get<ArrayType>(baseType.type_data);
    auto indexVal = mlir::arith::IndexCastOp::create(
        builder, location, builder.getIndexType(), idxVal);
    emitBoundsCheck(indexVal, arrType.get_size(), location);
    arr = ensurePointer(arr, location);
    return emitPtrArrayGEP(arr, idxVal, arrType, location);
  }

  imm_error("expression is not assignable", ast->get_location());
}

mlir::Value MLIRGenImpl::ensurePointer(mlir::Value val, mlir::Location loc) {
  if (!mlir::isa<mlir::LLVM::LLVMPointerType>(val.getType())) {
    auto alloca = emitAllocaOne(val.getType(), loc);
    mlir::LLVM::StoreOp::create(builder, loc, val, alloca);
    return alloca;
  }
  return val;
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
  case TypeKind::F32_t:
    return 4;
  case TypeKind::Bool:
  case TypeKind::Char:
    return 1;
  case TypeKind::Unit:
    return 0;
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
      size = static_cast<int64_t>(llvm::alignTo(size, fieldSize));
      size += fieldSize;
    }
    // Align total to largest field alignment
    if (!st.get_field_types().empty()) {
      int64_t maxAlign = 0;
      for (auto &ft : st.get_field_types())
        maxAlign = std::max(maxAlign, getTypeSize(ft));
      size = static_cast<int64_t>(llvm::alignTo(size, maxAlign));
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
    for (auto &vi : et.get_variants())
      max_payload = std::max(max_payload, getVariantPayloadSize(vi.payload_types));
    return 4 + max_payload; // i32 tag + payload
  }
  case TypeKind::Tuple: {
    auto &tt = std::get<TupleType>(type.type_data);
    int64_t size = 0;
    for (auto &et : tt.get_element_types()) {
      int64_t fieldSize = getTypeSize(et);
      size = static_cast<int64_t>(llvm::alignTo(size, fieldSize));
      size += fieldSize;
    }
    if (!tt.get_element_types().empty()) {
      int64_t maxAlign = 0;
      for (auto &et : tt.get_element_types())
        maxAlign = std::max(maxAlign, getTypeSize(et));
      size = static_cast<int64_t>(llvm::alignTo(size, maxAlign));
    }
    return size;
  }
  case TypeKind::NonExistent:
    imm_error("getTypeSize: type was never resolved (NonExistent) — "
              "this is a compiler bug: a type expression was not "
              "visited during type checking");
  case TypeKind::Poisoned:
    imm_error("getTypeSize: poisoned type reached codegen — "
              "a type error should have been reported and "
              "compilation halted before codegen");
  case TypeKind::Generic:
    imm_error("getTypeSize: generic template type reached codegen — "
              "this is a compiler bug: the generic definition was "
              "not monomorphized before codegen");
  case TypeKind::TypeParam:
    imm_error(fmt::format(
        "getTypeSize: unresolved type parameter '{}' reached codegen — "
        "this is a compiler bug: the type parameter was not substituted "
        "during monomorphization",
        type.to_string()));
  case TypeKind::Never:
    imm_error("getTypeSize: 'never' type reached codegen — "
              "this is a compiler bug: a diverging expression's "
              "type was not handled before codegen");
  }
}

int64_t MLIRGenImpl::getVariantPayloadSize(const std::vector<Type> &payload_types) {
  int64_t size = 0;
  for (auto &pt : payload_types) {
    int64_t fieldSize = getTypeSize(pt);
    size = static_cast<int64_t>(llvm::alignTo(size, fieldSize));
    size += fieldSize;
  }
  return size;
}

int64_t MLIRGenImpl::advancePayloadOffset(int64_t &byte_offset, const Type &field_type) {
  int64_t fieldSize = getTypeSize(field_type);
  byte_offset = static_cast<int64_t>(llvm::alignTo(byte_offset, fieldSize));
  int64_t offset = byte_offset;
  byte_offset += fieldSize;
  return offset;
}

mlir::Value
MLIRGenImpl::emitGlobalConstArray(AST::ArrayLiteralExprAST *arrLit,
                                  const Type &type, mlir::Location location) {
  auto arrType = std::get<ArrayType>(type.type_data);
  auto elemType = convertType(arrType.get_element());
  auto llvmArrayType =
      mlir::LLVM::LLVMArrayType::get(elemType, arrType.get_size());

  auto name =
      fmt::format("{}{}", kConstArrayPrefix.str(), constArrayCounter++);

  // Build the global constant in the module scope, then return to caller scope
  {
    mlir::OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(theModule.getBody());

    auto globalOp = mlir::LLVM::GlobalOp::create(
        builder, location, llvmArrayType, /*isConstant=*/true,
        mlir::LLVM::Linkage::Internal, name, mlir::Attribute{});

    // Create an initializer region: poison + insertvalue for each element
    auto &region = globalOp.getInitializerRegion();
    auto *block = builder.createBlock(&region);
    builder.setInsertionPointToStart(block);

    auto poison =
        mlir::LLVM::PoisonOp::create(builder, location, llvmArrayType);
    mlir::Value current = poison;
    for (size_t i = 0; i < arrLit->elements.size(); ++i) {
      auto *elem = arrLit->elements[i].get();
      mlir::Value elemVal;
      if (auto *num = llvm::dyn_cast<AST::NumberExprAST>(elem)) {
        auto elemSammineType = num->get_type();
        if (isFloatType(elemSammineType)) {
          auto mlirFloatType = mlir::cast<mlir::FloatType>(elemType);
          double val = std::stod(num->number);
          llvm::APFloat apVal(val);
          if (elemSammineType.type_kind == TypeKind::F32_t) {
            bool losesInfo = false;
            apVal.convert(llvm::APFloat::IEEEsingle(),
                          llvm::APFloat::rmNearestTiesToEven, &losesInfo);
          }
          elemVal = mlir::arith::ConstantFloatOp::create(
              builder, location, mlirFloatType, apVal);
        } else {
          int64_t val = std::stoll(num->number);
          elemVal = mlir::arith::ConstantIntOp::create(builder, location,
                                                       elemType, val);
        }
      } else if (auto *b = llvm::dyn_cast<AST::BoolExprAST>(elem)) {
        elemVal = mlir::arith::ConstantIntOp::create(builder, location,
                                                     b->b ? 1 : 0, 1);
      } else if (auto *c = llvm::dyn_cast<AST::CharExprAST>(elem)) {
        elemVal = mlir::arith::ConstantIntOp::create(
            builder, location, static_cast<uint8_t>(c->value), 8);
      }
      current = mlir::LLVM::InsertValueOp::create(
          builder, location, current, elemVal,
          llvm::ArrayRef<int64_t>{static_cast<int64_t>(i)});
    }
    mlir::LLVM::ReturnOp::create(builder, location, current);
  } // guard restores insertion point here

  return mlir::LLVM::AddressOfOp::create(builder, location, llvmPtrTy(), name);
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
        const std::string &sourceText, const AST::ASTProperties &props) {
  return MLIRGenImpl(context, moduleName, fileName, sourceText, props)
      .generate(program);
}

} // namespace sammine_lang
