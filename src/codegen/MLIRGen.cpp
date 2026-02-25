#include "codegen/MLIRGen.h"
#include "codegen/MLIRGenImpl.h"

#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include "fmt/core.h"

#include <string>

namespace sammine_lang {

// ===--- generate() ---===

mlir::ModuleOp MLIRGenImpl::generate(AST::ProgramAST *program) {
  theModule = mlir::ModuleOp::create(builder.getUnknownLoc());

  declareRuntimeFunctions();

  // Register the closure struct type: { ptr, ptr }
  auto closurePtrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
  closureType = mlir::LLVM::LLVMStructType::getIdentified(
      builder.getContext(), "sammine.closure");
  (void)closureType.setBody({closurePtrTy, closurePtrTy}, /*isPacked=*/false);

  // Pre-pass: register struct types before emitting any definitions,
  // since function prototypes may reference struct types.
  for (auto &def : program->DefinitionVec) {
    if (auto *sd = dynamic_cast<AST::StructDefAST *>(def.get())) {
      if (sd->type.type_kind != TypeKind::Poisoned) {
        auto &st = std::get<StructType>(sd->type.type_data);
        llvm::SmallVector<mlir::Type> fieldTypes;
        for (auto &ft : st.get_field_types())
          fieldTypes.push_back(convertType(ft));
        auto structTy = mlir::LLVM::LLVMStructType::getIdentified(
            builder.getContext(), "sammine.struct." + st.get_name());
        (void)structTy.setBody(fieldTypes, /*isPacked=*/false);
        structTypes[st.get_name()] = structTy;
      }
    }
  }

  // Forward-declare all non-generic functions and typeclass instance methods
  // so that references can find them even if the definition comes later.
  for (auto &def : program->DefinitionVec) {
    if (auto *fd = dynamic_cast<AST::FuncDefAST *>(def.get())) {
      if (fd->Prototype->is_generic())
        continue;
      forwardDeclareFunc(fd->Prototype.get());
    } else if (auto *tci =
                   dynamic_cast<AST::TypeClassInstanceAST *>(def.get())) {
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
  (void)ast;
  return builder.getUnknownLoc();
}

// ===--- Runtime function declarations ---===

void MLIRGenImpl::declareRuntimeFunctions() {
  auto unknownLoc = builder.getUnknownLoc();
  auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
  auto i32Ty = builder.getI32Type();
  auto i64Ty = builder.getI64Type();
  auto voidTy = mlir::LLVM::LLVMVoidType::get(builder.getContext());

  builder.setInsertionPointToEnd(theModule.getBody());

  // @malloc(i64) -> !llvm.ptr  [LLVM dialect — used by emitAllocExpr]
  {
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(ptrTy, {i64Ty});
    mlir::LLVM::LLVMFuncOp::create(builder, unknownLoc, "malloc", fnTy);
  }
  // @free(!llvm.ptr) -> void  [LLVM dialect — used by emitFreeExpr]
  {
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(voidTy, {ptrTy});
    mlir::LLVM::LLVMFuncOp::create(builder, unknownLoc, "free", fnTy);
  }
  // @exit(i32)  [func dialect — used by bounds check via func.call]
  {
    auto funcType = builder.getFunctionType({i32Ty}, {});
    auto funcOp = mlir::func::FuncOp::create(unknownLoc, "exit", funcType);
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
    theModule.push_back(funcOp);
  }
}

// ===--- Type conversion ---===

mlir::Type MLIRGenImpl::convertType(const Type &type) {
  switch (type.type_kind) {
  case TypeKind::I32_t:
    return builder.getI32Type();
  case TypeKind::I64_t:
    return builder.getI64Type();
  case TypeKind::F64_t:
    return builder.getF64Type();
  case TypeKind::Bool:
    return builder.getI1Type();
  case TypeKind::Unit:
    return mlir::NoneType::get(builder.getContext());
  case TypeKind::Integer:
    return builder.getI32Type();
  case TypeKind::Flt:
    return builder.getF64Type();
  case TypeKind::String:
    // String is a pointer to i8 in LLVM
    return mlir::LLVM::LLVMPointerType::get(builder.getContext());
  case TypeKind::Pointer:
    return mlir::LLVM::LLVMPointerType::get(builder.getContext());
  case TypeKind::Function:
    // Function-typed values are represented as closure fat pointers
    return closureType;
  case TypeKind::Array: {
    auto &arrType = std::get<ArrayType>(type.type_data);
    return mlir::MemRefType::get({static_cast<int64_t>(arrType.get_size())},
                                 convertType(arrType.get_element()));
  }
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(type.type_data);
    return structTypes.at(st.get_name());
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
         type.type_kind == TypeKind::Integer;
}

bool MLIRGenImpl::isFloatType(const Type &type) {
  return type.type_kind == TypeKind::F64_t || type.type_kind == TypeKind::Flt;
}

bool MLIRGenImpl::isBoolType(const Type &type) {
  return type.type_kind == TypeKind::Bool;
}

// ===--- Definition emission ---===

void MLIRGenImpl::emitDefinition(AST::DefinitionAST *def) {
  if (auto *funcDef = dynamic_cast<AST::FuncDefAST *>(def))
    emitFunction(funcDef);
  else if (auto *externDef = dynamic_cast<AST::ExternAST *>(def))
    emitExtern(externDef);
  else if (dynamic_cast<AST::StructDefAST *>(def))
    ; // Struct types registered in generate() pre-pass
  else if (dynamic_cast<AST::TypeClassDeclAST *>(def))
    ; // Skip typeclass declarations (no codegen needed)
  else if (auto *tci = dynamic_cast<AST::TypeClassInstanceAST *>(def)) {
    // Emit each instance method as a regular function
    for (auto &method : tci->methods)
      emitFunction(method.get());
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
      // sret: array returns become a trailing memref parameter, void return
      argTypes.push_back(convertType(retType));
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
  if (auto *num = dynamic_cast<AST::NumberExprAST *>(ast))
    return emitNumberExpr(num);
  if (auto *boolE = dynamic_cast<AST::BoolExprAST *>(ast))
    return emitBoolExpr(boolE);
  if (auto *unit = dynamic_cast<AST::UnitExprAST *>(ast))
    return emitUnitExpr(unit);
  if (auto *var = dynamic_cast<AST::VariableExprAST *>(ast))
    return emitVariableExpr(var);
  if (auto *bin = dynamic_cast<AST::BinaryExprAST *>(ast))
    return emitBinaryExpr(bin);
  if (auto *call = dynamic_cast<AST::CallExprAST *>(ast))
    return emitCallExpr(call);
  if (auto *ret = dynamic_cast<AST::ReturnExprAST *>(ast))
    return emitReturnExpr(ret);
  if (auto *varDef = dynamic_cast<AST::VarDefAST *>(ast))
    return emitVarDef(varDef);
  if (auto *ifE = dynamic_cast<AST::IfExprAST *>(ast))
    return emitIfExpr(ifE);
  if (auto *neg = dynamic_cast<AST::UnaryNegExprAST *>(ast))
    return emitUnaryNegExpr(neg);
  if (auto *str = dynamic_cast<AST::StringExprAST *>(ast))
    return emitStringExpr(str);
  if (auto *arrLit = dynamic_cast<AST::ArrayLiteralExprAST *>(ast))
    return emitArrayLiteralExpr(arrLit);
  if (auto *idx = dynamic_cast<AST::IndexExprAST *>(ast))
    return emitIndexExpr(idx);
  if (auto *len = dynamic_cast<AST::LenExprAST *>(ast))
    return emitLenExpr(len);
  if (auto *deref = dynamic_cast<AST::DerefExprAST *>(ast))
    return emitDerefExpr(deref);
  if (auto *addrOf = dynamic_cast<AST::AddrOfExprAST *>(ast))
    return emitAddrOfExpr(addrOf);
  if (auto *alloc = dynamic_cast<AST::AllocExprAST *>(ast))
    return emitAllocExpr(alloc);
  if (auto *freeE = dynamic_cast<AST::FreeExprAST *>(ast))
    return emitFreeExpr(freeE);
  if (auto *structLit = dynamic_cast<AST::StructLiteralExprAST *>(ast))
    return emitStructLiteralExpr(structLit);
  if (auto *fieldAccess = dynamic_cast<AST::FieldAccessExprAST *>(ast))
    return emitFieldAccessExpr(fieldAccess);

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
  mlir::Value initVal = emitExpr(ast->Expression.get());
  if (!initVal)
    return nullptr;

  // Arrays: the literal already returns a memref<NxT>, register it directly
  if (ast->type.type_kind == TypeKind::Array) {
    symbolTable.registerNameT(ast->TypedVar->name, initVal);
    return initVal;
  }

  // All non-array variables: llvm.alloca + llvm.store (uniform model)
  auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
  auto elemType = convertType(ast->type);
  auto one = mlir::arith::ConstantIntOp::create(builder, loc(ast),
                                                  builder.getI64Type(), 1);
  auto alloca =
      mlir::LLVM::AllocaOp::create(builder, loc(ast), ptrTy, elemType, one);
  mlir::LLVM::StoreOp::create(builder, loc(ast), initVal, alloca);
  symbolTable.registerNameT(ast->TypedVar->name, alloca);
  return initVal;
}

// ===--- Helpers ---===

int64_t MLIRGenImpl::getTypeSize(const Type &type) {
  switch (type.type_kind) {
  case TypeKind::I32_t:
  case TypeKind::Integer:
    return 4;
  case TypeKind::I64_t:
    return 8;
  case TypeKind::F64_t:
  case TypeKind::Flt:
    return 8;
  case TypeKind::Bool:
    return 1;
  case TypeKind::Pointer:
  case TypeKind::String:
    return 8;
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(type.type_data);
    int64_t size = 0;
    for (auto &ft : st.get_field_types())
      size += getTypeSize(ft);
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

    auto savedIP = builder.saveInsertionPoint();
    builder.setInsertionPointToStart(theModule.getBody());

    auto strType = mlir::LLVM::LLVMArrayType::get(
        mlir::IntegerType::get(builder.getContext(), 8),
        nullTerminated.size());
    globalOp = mlir::LLVM::GlobalOp::create(
        builder, location, strType, /*isConstant=*/true,
        mlir::LLVM::Linkage::Internal, name,
        builder.getStringAttr(nullTerminated));

    builder.restoreInsertionPoint(savedIP);
  }

  // Get address of the global
  auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
  return mlir::LLVM::AddressOfOp::create(builder, location, ptrTy, name);
}

// ===--- Public entry point ---===

mlir::OwningOpRef<mlir::ModuleOp>
mlirGen(mlir::MLIRContext &context, AST::ProgramAST *program,
        const std::string &moduleName) {
  return MLIRGenImpl(context, moduleName).generate(program);
}

} // namespace sammine_lang
