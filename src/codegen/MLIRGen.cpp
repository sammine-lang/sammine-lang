#include "codegen/MLIRGen.h"

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
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include "fmt/core.h"
#include "util/Utilities.h"

#include <string>

namespace sammine_lang {
namespace {

/// Implementation class for MLIR generation from a sammine AST.
class MLIRGenImpl {
public:
  MLIRGenImpl(mlir::MLIRContext &context, const std::string &moduleName)
      : builder(&context), moduleName(moduleName) {}

  mlir::ModuleOp generate(AST::ProgramAST *program) {
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

private:
  mlir::ModuleOp theModule;
  mlir::OpBuilder builder;
  std::string moduleName;

  // Reuse the project's LexicalStack for scoped variable tracking
  AST::LexicalStack<mlir::Value, std::monostate> symbolTable;

  int strCounter = 0;

  // Registered struct types (name -> LLVM struct type)
  std::map<std::string, mlir::LLVM::LLVMStructType> structTypes;

  // Closure support: { code_ptr, env_ptr } fat pointer
  mlir::LLVM::LLVMStructType closureType;
  std::map<std::string, std::string> closureWrappers;
  int partialCounter = 0;

  // sret: when a function returns an array, the caller passes an extra
  // memref parameter and the callee copies into it instead of returning.
  mlir::Value currentSretBuffer = nullptr;

  // ===--- Location helpers ---===

  mlir::Location loc(AST::AstBase *ast) {
    // For now, use unknown loc; we can add FileLineColLoc later when we
    // integrate source location tracking.
    (void)ast;
    return builder.getUnknownLoc();
  }

  // ===--- Runtime function declarations ---===

  void declareRuntimeFunctions() {
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

  mlir::Type convertType(const Type &type) {
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

  bool isIntegerType(const Type &type) {
    return type.type_kind == TypeKind::I32_t ||
           type.type_kind == TypeKind::I64_t ||
           type.type_kind == TypeKind::Integer;
  }

  bool isFloatType(const Type &type) {
    return type.type_kind == TypeKind::F64_t || type.type_kind == TypeKind::Flt;
  }

  bool isBoolType(const Type &type) {
    return type.type_kind == TypeKind::Bool;
  }

  // ===--- Definition emission ---===

  void emitDefinition(AST::DefinitionAST *def) {
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

  mlir::FunctionType buildFuncType(AST::PrototypeAST *proto) {
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

  std::string mangleName(const sammine_util::QualifiedName &qn) {
    return qn.with_module(moduleName).mangled();
  }

  void forwardDeclareFunc(AST::PrototypeAST *proto) {
    if (proto->is_generic())
      return;
    auto funcType = buildFuncType(proto);
    auto funcName = mangleName(proto->functionName);
    if (!theModule.lookupSymbol(funcName)) {
      builder.setInsertionPointToEnd(theModule.getBody());
      auto funcOp = mlir::func::FuncOp::create(builder.getUnknownLoc(),
                                                 funcName, funcType);
      funcOp.setVisibility(moduleName.empty()
                               ? mlir::SymbolTable::Visibility::Private
                               : mlir::SymbolTable::Visibility::Public);
      theModule.push_back(funcOp);
    }
  }

  // ===--- Closure helpers ---===

  /// Build a closure value: { code_ptr, env_ptr }
  mlir::Value buildClosure(mlir::Value codePtr, mlir::Value envPtr,
                           mlir::Location loc) {
    mlir::Value closure =
        mlir::LLVM::UndefOp::create(builder, loc, closureType);
    closure = mlir::LLVM::InsertValueOp::create(
        builder, loc, closure, codePtr, llvm::ArrayRef<int64_t>{0});
    closure = mlir::LLVM::InsertValueOp::create(
        builder, loc, closure, envPtr, llvm::ArrayRef<int64_t>{1});
    return closure;
  }

  /// Get the LLVM function type for a closure wrapper: ret(ptr, params...)
  mlir::LLVM::LLVMFunctionType getClosureFuncType(const FunctionType &ft) {
    auto retType = ft.get_return_type();
    if (retType.type_kind == TypeKind::Array)
      sammine_util::abort("MLIRGen: closure returning array not supported");

    auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
    llvm::SmallVector<mlir::Type> params;
    params.push_back(ptrTy); // env ptr
    for (auto &p : ft.get_params_types())
      params.push_back(convertType(p));
    if (retType.type_kind == TypeKind::Unit)
      return mlir::LLVM::LLVMFunctionType::get(
          mlir::LLVM::LLVMVoidType::get(builder.getContext()), params);
    return mlir::LLVM::LLVMFunctionType::get(convertType(retType), params);
  }

  /// Create (or cache) a closure wrapper for a named function:
  /// llvm.func @__wrap_<name>(ptr %env, params...) that forwards to @<name>
  std::string getOrCreateClosureWrapper(const std::string &funcName,
                                        const FunctionType &ft) {
    auto it = closureWrappers.find(funcName);
    if (it != closureWrappers.end())
      return it->second;

    auto wrapperName = "__wrap_" + funcName;
    auto wrapperFnTy = getClosureFuncType(ft);
    auto wrapperLoc = builder.getUnknownLoc();

    auto savedIP = builder.saveInsertionPoint();
    builder.setInsertionPointToEnd(theModule.getBody());

    auto wrapperOp = mlir::LLVM::LLVMFuncOp::create(
        builder, wrapperLoc, wrapperName, wrapperFnTy,
        mlir::LLVM::Linkage::Internal);
    auto &entryBlock = *wrapperOp.addEntryBlock(builder);
    builder.setInsertionPointToStart(&entryBlock);

    // Forward all args except env (arg 0) to the original func.func
    llvm::SmallVector<mlir::Value> forwardArgs;
    for (size_t i = 1; i < entryBlock.getNumArguments(); i++)
      forwardArgs.push_back(entryBlock.getArgument(i));

    auto retType = ft.get_return_type();
    if (retType.type_kind == TypeKind::Unit) {
      mlir::func::CallOp::create(builder, wrapperLoc, funcName,
                                  mlir::TypeRange{}, forwardArgs);
      mlir::LLVM::ReturnOp::create(builder, wrapperLoc, mlir::ValueRange{});
    } else {
      auto callOp = mlir::func::CallOp::create(
          builder, wrapperLoc, funcName,
          mlir::TypeRange{convertType(retType)}, forwardArgs);
      mlir::LLVM::ReturnOp::create(builder, wrapperLoc, callOp.getResults());
    }

    builder.restoreInsertionPoint(savedIP);
    closureWrappers[funcName] = wrapperName;
    return wrapperName;
  }

  void emitFunction(AST::FuncDefAST *ast) {
    // Skip generic function templates — only monomorphized copies
    if (ast->Prototype->is_generic())
      return;

    symbolTable.push_context();

    auto funcType = buildFuncType(ast->Prototype.get());
    std::string funcName = mangleName(ast->Prototype->functionName);

    // Look up forward-declared function, or create a new one
    mlir::func::FuncOp funcOp;
    if (auto existing =
            theModule.lookupSymbol<mlir::func::FuncOp>(funcName)) {
      funcOp = existing;
    } else {
      builder.setInsertionPointToEnd(theModule.getBody());
      funcOp = mlir::func::FuncOp::create(loc(ast),
                                            llvm::StringRef(funcName),
                                            funcType);
      theModule.push_back(funcOp);
    }
    // Make main and library-module functions public, everything else private
    if (funcName == "main" || !moduleName.empty())
      funcOp.setVisibility(mlir::SymbolTable::Visibility::Public);
    else
      funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);

    // Create entry block with arguments
    auto &entryBlock = *funcOp.addEntryBlock();

    builder.setInsertionPointToStart(&entryBlock);

    // Determine if this function returns an array (sret transform)
    bool isSret = false;
    if (ast->Prototype->type.type_kind == TypeKind::Function) {
      auto &ft = std::get<FunctionType>(ast->Prototype->type.type_data);
      isSret = ft.get_return_type().type_kind == TypeKind::Array;
    }

    // Bind parameters in symbol table
    // All non-array params get llvm.alloca + llvm.store (uniform model)
    for (size_t i = 0; i < ast->Prototype->parameterVectors.size(); ++i) {
      auto &param = ast->Prototype->parameterVectors[i];
      auto argVal = entryBlock.getArgument(i);
      if (mlir::isa<mlir::MemRefType>(argVal.getType())) {
        // Array/sret params: register memref directly
        symbolTable.registerNameT(param->name, argVal);
      } else {
        auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
        auto one = mlir::arith::ConstantIntOp::create(builder, loc(ast),
                                                        builder.getI64Type(), 1);
        auto alloca = mlir::LLVM::AllocaOp::create(builder, loc(ast), ptrTy,
                                                     argVal.getType(), one);
        mlir::LLVM::StoreOp::create(builder, loc(ast), argVal, alloca);
        symbolTable.registerNameT(param->name, alloca);
      }
    }

    // If sret, the last block argument is the output buffer
    if (isSret)
      currentSretBuffer = entryBlock.getArgument(entryBlock.getNumArguments() - 1);

    // Emit the function body
    mlir::Value bodyResult = emitBlock(ast->Block.get());

    // If the block didn't end with a return, insert one.
    // Check if the current block already has a terminator.
    auto *currentBlock = builder.getInsertionBlock();
    if (currentBlock->empty() ||
        !currentBlock->back().hasTrait<mlir::OpTrait::IsTerminator>()) {
      if (isSret && bodyResult) {
        // sret: copy array into output buffer, return void
        mlir::memref::CopyOp::create(builder, loc(ast), bodyResult,
                                      currentSretBuffer);
        mlir::func::ReturnOp::create(builder, loc(ast));
      } else if (funcType.getNumResults() == 0) {
        mlir::func::ReturnOp::create(builder, loc(ast));
      } else if (bodyResult) {
        mlir::func::ReturnOp::create(builder, loc(ast),
                                     mlir::ValueRange{bodyResult});
      } else {
        // If we have a return type but no result, something went wrong.
        // Emit a return with a zero/null constant as fallback.
        auto retType = funcType.getResult(0);
        mlir::Value zeroVal;
        if (mlir::isa<mlir::LLVM::LLVMPointerType>(retType))
          zeroVal = mlir::LLVM::ZeroOp::create(builder, loc(ast), retType);
        else
          zeroVal =
              mlir::arith::ConstantIntOp::create(
                  builder, loc(ast), 0, retType.getIntOrFloatBitWidth())
                  .getResult();
        mlir::func::ReturnOp::create(builder, loc(ast),
                                     mlir::ValueRange{zeroVal});
      }
    }

    currentSretBuffer = nullptr;
    symbolTable.pop_context();
  }

  void emitExtern(AST::ExternAST *ast) {
    std::string funcName = ast->Prototype->functionName.mangled();

    // Skip if already declared (e.g. by declareRuntimeFunctions)
    if (theModule.lookupSymbol(funcName))
      return;

    builder.setInsertionPointToEnd(theModule.getBody());

    if (ast->Prototype->is_var_arg) {
      // Vararg C functions must use LLVM dialect (func dialect has no varargs)
      llvm::SmallVector<mlir::Type, 4> paramTypes;
      for (auto &param : ast->Prototype->parameterVectors)
        paramTypes.push_back(convertType(param->type));
      // Return type
      auto &funcTypeData = std::get<FunctionType>(ast->Prototype->type.type_data);
      auto retType = funcTypeData.get_return_type();
      mlir::Type llvmRetType;
      if (retType.type_kind == TypeKind::Unit)
        llvmRetType = mlir::LLVM::LLVMVoidType::get(builder.getContext());
      else
        llvmRetType = convertType(retType);
      auto fnTy = mlir::LLVM::LLVMFunctionType::get(
          llvmRetType, paramTypes, /*isVarArg=*/true);
      mlir::LLVM::LLVMFuncOp::create(builder, loc(ast), funcName, fnTy);
    } else {
      auto funcType = buildFuncType(ast->Prototype.get());
      auto funcOp = mlir::func::FuncOp::create(
          loc(ast), llvm::StringRef(funcName), funcType);
      funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
      theModule.push_back(funcOp);
    }
  }

  // ===--- Expression emission ---===

  mlir::Value emitExpr(AST::ExprAST *ast) {
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

  mlir::Value emitNumberExpr(AST::NumberExprAST *ast) {
    auto location = loc(ast);
    const auto &type = ast->type;

    if (isFloatType(type)) {
      auto mlirType = mlir::cast<mlir::FloatType>(convertType(type));
      double val = std::stod(ast->number);
      return mlir::arith::ConstantFloatOp::create(
                 builder, location, mlirType, llvm::APFloat(val))
          .getResult();
    }

    // Integer
    auto mlirType = convertType(type);
    int64_t val = std::stoll(ast->number);
    return mlir::arith::ConstantIntOp::create(builder, location, mlirType, val)
        .getResult();
  }

  mlir::Value emitBoolExpr(AST::BoolExprAST *ast) {
    return mlir::arith::ConstantIntOp::create(builder, loc(ast),
                                              ast->b ? 1 : 0, 1)
        .getResult();
  }

  mlir::Value emitUnitExpr(AST::UnitExprAST *) {
    // Unit has no runtime representation. Return nullptr — callers
    // that need a value should handle this.
    return nullptr;
  }

  mlir::Value emitVariableExpr(AST::VariableExprAST *ast) {
    // If the variable is in the symbol table, handle it normally
    if (symbolTable.queryName(ast->variableName) != nameNotFound) {
      auto val = symbolTable.get_from_name(ast->variableName);

      // Array (memref<NxT>): return the memref itself
      if (mlir::isa<mlir::MemRefType>(val.getType()))
        return val;

      // Variable in llvm.alloca: load the value
      if (mlir::isa<mlir::LLVM::LLVMPointerType>(val.getType())) {
        auto elemType = convertType(ast->type);
        return mlir::LLVM::LoadOp::create(builder, loc(ast), elemType, val);
      }

      return val;
    }

    // Not in symbol table — check if it's a module-level function used as a value
    if (ast->type.type_kind == TypeKind::Function) {
      auto funcName = ast->variableName;
      if (!theModule.lookupSymbol<mlir::func::FuncOp>(funcName))
        funcName = mangleName(sammine_util::QualifiedName::local(funcName));
      if (theModule.lookupSymbol<mlir::func::FuncOp>(funcName)) {
        auto &ft = std::get<FunctionType>(ast->type.type_data);
        auto wrapperName = getOrCreateClosureWrapper(funcName, ft);
        auto ptrTy =
            mlir::LLVM::LLVMPointerType::get(builder.getContext());
        auto wrapperAddr = mlir::LLVM::AddressOfOp::create(
            builder, loc(ast), ptrTy, wrapperName);
        auto nullEnv =
            mlir::LLVM::ZeroOp::create(builder, loc(ast), ptrTy);
        return buildClosure(wrapperAddr, nullEnv, loc(ast));
      }
    }

    sammine_util::abort(
        fmt::format("MLIRGen: unknown variable '{}'", ast->variableName));
  }

  mlir::Value emitBinaryExpr(AST::BinaryExprAST *ast) {
    // Assignment: store RHS into LHS's memref
    if (ast->Op->is_assign()) {
      // *p = val
      if (auto *deref = dynamic_cast<AST::DerefExprAST *>(ast->LHS.get())) {
        auto ptr = emitExpr(deref->operand.get());
        auto rhs = emitExpr(ast->RHS.get());
        if (!ptr || !rhs)
          return nullptr;
        mlir::LLVM::StoreOp::create(builder, loc(ast), rhs, ptr);
        return rhs;
      }

      // arr[i] = val  (or (*ptr)[i] = val)
      if (auto *idxExpr =
              dynamic_cast<AST::IndexExprAST *>(ast->LHS.get())) {
        auto arr = emitExpr(idxExpr->array_expr.get());
        auto idx = emitExpr(idxExpr->index_expr.get());
        auto rhs = emitExpr(ast->RHS.get());
        if (!arr || !idx || !rhs)
          return nullptr;

        // If the array is a raw !llvm.ptr (from deref of ptr<[T;N]>),
        // use LLVM GEP + store.
        if (mlir::isa<mlir::LLVM::LLVMPointerType>(arr.getType())) {
          auto &arrType =
              std::get<ArrayType>(idxExpr->array_expr->type.type_data);
          emitPtrArrayStore(arr, idx, rhs, arrType, loc(ast));
          return rhs;
        }

        auto indexVal = mlir::arith::IndexCastOp::create(
            builder, loc(ast), builder.getIndexType(), idx);
        mlir::memref::StoreOp::create(builder, loc(ast), rhs, arr,
                                       mlir::ValueRange{indexVal});
        return rhs;
      }

      // x = val (mutable scalar)
      mlir::Value rhs = emitExpr(ast->RHS.get());
      if (!rhs)
        return nullptr;

      if (auto *lhsVar = dynamic_cast<AST::VariableExprAST *>(ast->LHS.get())) {
        auto varPtr = symbolTable.get_from_name(lhsVar->variableName);
        mlir::LLVM::StoreOp::create(builder, loc(ast), rhs, varPtr);
        return rhs;
      }

      sammine_util::abort("MLIRGen: unsupported assignment LHS");
    }

    mlir::Value lhs = emitExpr(ast->LHS.get());
    mlir::Value rhs = emitExpr(ast->RHS.get());
    if (!lhs || !rhs)
      return nullptr;

    auto location = loc(ast);
    const auto &resultType = ast->type;

    // Integer arithmetic
    if (isIntegerType(resultType)) {
      switch (ast->Op->tok_type) {
      case TokADD:
        return mlir::arith::AddIOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokSUB:
        return mlir::arith::SubIOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokMUL:
        return mlir::arith::MulIOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokDIV:
        return mlir::arith::DivSIOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokMOD:
        return mlir::arith::RemSIOp::create(builder, location, lhs, rhs)
            .getResult();
      default:
        break;
      }
    }

    // Float arithmetic
    if (isFloatType(resultType)) {
      switch (ast->Op->tok_type) {
      case TokADD:
        return mlir::arith::AddFOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokSUB:
        return mlir::arith::SubFOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokMUL:
        return mlir::arith::MulFOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokDIV:
        return mlir::arith::DivFOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokMOD:
        return mlir::arith::RemFOp::create(builder, location, lhs, rhs)
            .getResult();
      default:
        break;
      }
    }

    // Comparison operators — result type is Bool but operand types matter
    if (isBoolType(resultType)) {
      // Determine if comparing int or float from the LHS operand type
      const auto &lhsType = ast->LHS->type;

      if (isIntegerType(lhsType)) {
        mlir::arith::CmpIPredicate pred;
        switch (ast->Op->tok_type) {
        case TokEQUAL:
          pred = mlir::arith::CmpIPredicate::eq;
          break;
        case TokNOTEqual:
          pred = mlir::arith::CmpIPredicate::ne;
          break;
        case TokLESS:
          pred = mlir::arith::CmpIPredicate::slt;
          break;
        case TokLessEqual:
          pred = mlir::arith::CmpIPredicate::sle;
          break;
        case TokGREATER:
          pred = mlir::arith::CmpIPredicate::sgt;
          break;
        case TokGreaterEqual:
          pred = mlir::arith::CmpIPredicate::sge;
          break;
        case TokAND:
          return mlir::arith::AndIOp::create(builder, location, lhs, rhs)
              .getResult();
        case TokOR:
          return mlir::arith::OrIOp::create(builder, location, lhs, rhs)
              .getResult();
        default:
          sammine_util::abort(fmt::format(
              "MLIRGen: unsupported integer comparison operator"));
        }
        return mlir::arith::CmpIOp::create(builder, location, pred, lhs, rhs)
            .getResult();
      }

      if (isFloatType(lhsType)) {
        mlir::arith::CmpFPredicate pred;
        switch (ast->Op->tok_type) {
        case TokEQUAL:
          pred = mlir::arith::CmpFPredicate::OEQ;
          break;
        case TokNOTEqual:
          pred = mlir::arith::CmpFPredicate::ONE;
          break;
        case TokLESS:
          pred = mlir::arith::CmpFPredicate::OLT;
          break;
        case TokLessEqual:
          pred = mlir::arith::CmpFPredicate::OLE;
          break;
        case TokGREATER:
          pred = mlir::arith::CmpFPredicate::OGT;
          break;
        case TokGreaterEqual:
          pred = mlir::arith::CmpFPredicate::OGE;
          break;
        default:
          sammine_util::abort(fmt::format(
              "MLIRGen: unsupported float comparison operator"));
        }
        return mlir::arith::CmpFOp::create(builder, location, pred, lhs, rhs)
            .getResult();
      }

      // Pointer comparisons (== and !=)
      if (lhsType.type_kind == TypeKind::Pointer) {
        auto i64Ty = builder.getI64Type();
        auto lhsInt = mlir::LLVM::PtrToIntOp::create(builder, location,
                                                       i64Ty, lhs);
        auto rhsInt = mlir::LLVM::PtrToIntOp::create(builder, location,
                                                       i64Ty, rhs);
        mlir::arith::CmpIPredicate pred;
        switch (ast->Op->tok_type) {
        case TokEQUAL:
          pred = mlir::arith::CmpIPredicate::eq;
          break;
        case TokNOTEqual:
          pred = mlir::arith::CmpIPredicate::ne;
          break;
        default:
          sammine_util::abort("MLIRGen: only == and != supported for pointers");
        }
        return mlir::arith::CmpIOp::create(builder, location, pred,
                                             lhsInt, rhsInt)
            .getResult();
      }

      // Array comparisons (== and !=) — element-by-element
      if (lhsType.type_kind == TypeKind::Array) {
        return emitArrayComparison(lhs, rhs, lhsType, ast->Op->tok_type,
                                   location);
      }

      // Bool comparisons (== and !=)
      if (isBoolType(lhsType)) {
        mlir::arith::CmpIPredicate pred;
        switch (ast->Op->tok_type) {
        case TokEQUAL:
          pred = mlir::arith::CmpIPredicate::eq;
          break;
        case TokNOTEqual:
          pred = mlir::arith::CmpIPredicate::ne;
          break;
        case TokAND:
          return mlir::arith::AndIOp::create(builder, location, lhs, rhs)
              .getResult();
        case TokOR:
          return mlir::arith::OrIOp::create(builder, location, lhs, rhs)
              .getResult();
        default:
          sammine_util::abort("MLIRGen: unsupported bool operator");
        }
        return mlir::arith::CmpIOp::create(builder, location, pred, lhs, rhs)
            .getResult();
      }
    }

    sammine_util::abort(
        fmt::format("MLIRGen: unsupported binary operator '{}'",
                    ast->Op->lexeme));
  }

  /// Emit partial application: build env struct with bound args, generate
  /// a wrapper llvm.func, return a closure { wrapper, env }.
  mlir::Value emitPartialApplication(AST::CallExprAST *ast,
                                      const std::string &calleeName,
                                      llvm::ArrayRef<mlir::Value> boundArgs) {
    auto location = loc(ast);
    auto &fullFt =
        std::get<FunctionType>(ast->callee_func_type->type_data);
    auto allParams = fullFt.get_params_types();
    size_t boundCount = boundArgs.size();
    auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());

    // 1. Create env struct type and stack-allocate it
    llvm::SmallVector<mlir::Type> envFields;
    for (size_t i = 0; i < boundCount; i++)
      envFields.push_back(convertType(allParams[i]));
    auto envStructTy =
        mlir::LLVM::LLVMStructType::getLiteral(builder.getContext(),
                                                envFields);

    auto one = mlir::arith::ConstantIntOp::create(builder, location,
                                                    builder.getI64Type(), 1);
    auto envAlloca = mlir::LLVM::AllocaOp::create(builder, location, ptrTy,
                                                    envStructTy, one);

    // Store bound args into env
    for (size_t i = 0; i < boundCount; i++) {
      auto gep = mlir::LLVM::GEPOp::create(
          builder, location, ptrTy, envStructTy, envAlloca,
          llvm::ArrayRef<mlir::LLVM::GEPArg>{0, static_cast<int32_t>(i)});
      mlir::LLVM::StoreOp::create(builder, location, boundArgs[i], gep);
    }

    // 2. Generate partial wrapper: llvm.func @__partial_N(ptr %env, remaining...)
    auto retType = fullFt.get_return_type();
    llvm::SmallVector<mlir::Type> wrapperParams;
    wrapperParams.push_back(ptrTy); // env
    for (size_t i = boundCount; i < allParams.size(); i++)
      wrapperParams.push_back(convertType(allParams[i]));

    mlir::Type llvmRet =
        retType.type_kind == TypeKind::Unit
            ? mlir::Type(
                  mlir::LLVM::LLVMVoidType::get(builder.getContext()))
            : convertType(retType);
    auto wrapperFnTy =
        mlir::LLVM::LLVMFunctionType::get(llvmRet, wrapperParams);
    auto wrapperName = fmt::format("__partial_{}", partialCounter++);

    auto savedIP = builder.saveInsertionPoint();
    builder.setInsertionPointToEnd(theModule.getBody());

    auto wrapperOp = mlir::LLVM::LLVMFuncOp::create(
        builder, location, wrapperName, wrapperFnTy,
        mlir::LLVM::Linkage::Internal);
    auto &entryBlock = *wrapperOp.addEntryBlock(builder);
    builder.setInsertionPointToStart(&entryBlock);

    // Load bound args from env
    auto envArg = entryBlock.getArgument(0);
    llvm::SmallVector<mlir::Value> fullArgs;
    for (size_t i = 0; i < boundCount; i++) {
      auto gep = mlir::LLVM::GEPOp::create(
          builder, location, ptrTy, envStructTy, envArg,
          llvm::ArrayRef<mlir::LLVM::GEPArg>{0, static_cast<int32_t>(i)});
      auto loaded = mlir::LLVM::LoadOp::create(builder, location,
                                                 envFields[i], gep);
      fullArgs.push_back(loaded);
    }
    // Forward remaining args
    for (size_t i = 1; i < entryBlock.getNumArguments(); i++)
      fullArgs.push_back(entryBlock.getArgument(i));

    // Call original function
    if (retType.type_kind == TypeKind::Unit) {
      mlir::func::CallOp::create(builder, location, calleeName,
                                  mlir::TypeRange{}, fullArgs);
      mlir::LLVM::ReturnOp::create(builder, location, mlir::ValueRange{});
    } else {
      auto callResult = mlir::func::CallOp::create(
          builder, location, calleeName,
          mlir::TypeRange{convertType(retType)}, fullArgs);
      mlir::LLVM::ReturnOp::create(builder, location,
                                    callResult.getResults());
    }

    builder.restoreInsertionPoint(savedIP);

    // 3. Build closure: { wrapper_addr, env_alloca }
    auto wrapperAddr = mlir::LLVM::AddressOfOp::create(builder, location,
                                                         ptrTy, wrapperName);
    return buildClosure(wrapperAddr, envAlloca, location);
  }

  /// Emit an indirect call through a closure variable.
  /// Extracts code_ptr and env_ptr, calls code_ptr(env_ptr, args...).
  mlir::Value emitIndirectCall(AST::CallExprAST *ast,
                                llvm::ArrayRef<mlir::Value> operands) {
    auto location = loc(ast);
    auto varName = ast->functionName.mangled();

    // Load closure value from symbol table
    auto closureVal = symbolTable.get_from_name(varName);

    // If stored in llvm.alloca (variable), load it first
    if (mlir::isa<mlir::LLVM::LLVMPointerType>(closureVal.getType()))
      closureVal = mlir::LLVM::LoadOp::create(builder, location,
                                                closureType, closureVal);

    // Extract code_ptr and env_ptr
    auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
    auto codePtr = mlir::LLVM::ExtractValueOp::create(
        builder, location, ptrTy, closureVal,
        llvm::ArrayRef<int64_t>{0});
    auto envPtr = mlir::LLVM::ExtractValueOp::create(
        builder, location, ptrTy, closureVal,
        llvm::ArrayRef<int64_t>{1});

    // Build call args: env_ptr, then user args
    llvm::SmallVector<mlir::Value> callArgs;
    callArgs.push_back(envPtr);
    callArgs.append(operands.begin(), operands.end());

    auto &ft =
        std::get<FunctionType>(ast->callee_func_type->type_data);
    auto closureFnTy = getClosureFuncType(ft);

    // For indirect LLVM calls, build the op manually:
    // the callee (function pointer) goes as the first operand,
    // followed by the actual arguments.
    llvm::SmallVector<mlir::Value> allOps;
    allOps.push_back(codePtr);
    allOps.append(callArgs.begin(), callArgs.end());

    auto retType = ft.get_return_type();
    llvm::SmallVector<mlir::Type> resultTypes;
    if (retType.type_kind != TypeKind::Unit)
      resultTypes.push_back(convertType(retType));

    // Indirect call: function pointer is first operand, no callee symbol
    // Use the (LLVMFunctionType, ValueRange) overload for indirect calls
    auto callOp = mlir::LLVM::CallOp::create(
        builder, location, closureFnTy, allOps);
    if (retType.type_kind == TypeKind::Unit)
      return nullptr;
    return callOp.getResult();
  }

  mlir::Value emitCallExpr(AST::CallExprAST *ast) {
    auto location = loc(ast);

    // Emit arguments
    llvm::SmallVector<mlir::Value, 4> operands;
    for (auto &arg : ast->arguments) {
      auto val = emitExpr(arg.get());
      if (val)
        operands.push_back(val);
    }

    // Resolve callee name — use the qualified name directly, matching
    // the LLVM backend. Names already include module prefixes where needed
    // (e.g. "math$add" for imports, "wrapper" for local calls).
    std::string callee;
    if (ast->resolved_generic_name.has_value())
      callee = *ast->resolved_generic_name;
    else
      callee = ast->functionName.mangled();

    // Imported C externs are declared with their base name (e.g. "printf")
    if (!theModule.lookupSymbol(callee)) {
      if (theModule.lookupSymbol(ast->functionName.name))
        callee = ast->functionName.name;
    }

    // Path 1: Direct call (not partial) — callee found in module
    if (theModule.lookupSymbol(callee) && !ast->is_partial) {
      // sret: if call returns an array, allocate local buffer, pass as extra arg
      if (ast->type.type_kind == TypeKind::Array) {
        auto arrMlirType = mlir::cast<mlir::MemRefType>(convertType(ast->type));
        auto localBuf = mlir::memref::AllocaOp::create(
            builder, location, arrMlirType);
        operands.push_back(localBuf);
        mlir::func::CallOp::create(
            builder, location, llvm::StringRef(callee),
            mlir::TypeRange{}, operands);
        return localBuf;
      }

      llvm::SmallVector<mlir::Type, 1> resultTypes;
      if (ast->type.type_kind != TypeKind::Unit)
        resultTypes.push_back(convertType(ast->type));

      // If callee is an llvm.func (vararg C function), use llvm.call
      if (auto llvmFunc =
              theModule.lookupSymbol<mlir::LLVM::LLVMFuncOp>(callee)) {
        auto llvmCallOp = mlir::LLVM::CallOp::create(
            builder, location, resultTypes, callee, operands);
        llvmCallOp.setVarCalleeType(llvmFunc.getFunctionType());
        if (llvmCallOp.getNumResults() > 0)
          return llvmCallOp.getResult();
        return nullptr;
      }

      auto callOp = mlir::func::CallOp::create(
          builder, location, llvm::StringRef(callee), resultTypes, operands);
      if (callOp.getNumResults() > 0)
        return callOp.getResult(0);
      return nullptr;
    }

    // Path 2: Partial application — callee found + is_partial
    if (theModule.lookupSymbol(callee) && ast->is_partial)
      return emitPartialApplication(ast, callee, operands);

    // Path 3: Indirect call through closure variable
    return emitIndirectCall(ast, operands);
  }

  mlir::Value emitReturnExpr(AST::ReturnExprAST *ast) {
    auto location = loc(ast);

    if (ast->return_expr) {
      auto val = emitExpr(ast->return_expr.get());
      if (currentSretBuffer && val) {
        // sret: copy array into output buffer, return void
        mlir::memref::CopyOp::create(builder, location, val,
                                      currentSretBuffer);
        mlir::func::ReturnOp::create(builder, location);
      } else if (val) {
        mlir::func::ReturnOp::create(builder, location,
                                     mlir::ValueRange{val});
      } else {
        mlir::func::ReturnOp::create(builder, location);
      }
    } else {
      mlir::func::ReturnOp::create(builder, location);
    }

    return nullptr;
  }

  mlir::Value emitBlock(AST::BlockAST *ast) {
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

  mlir::Value emitVarDef(AST::VarDefAST *ast) {
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

  mlir::Value emitIfExpr(AST::IfExprAST *ast) {
    auto location = loc(ast);
    auto cond = emitExpr(ast->bool_expr.get());
    if (!cond)
      return nullptr;

    bool hasResult = ast->type.type_kind != TypeKind::Unit &&
                     ast->type.type_kind != TypeKind::Never;

    auto *parentRegion = builder.getInsertionBlock()->getParent();

    // Create blocks: then, else, merge
    auto *thenBlock = new mlir::Block();
    auto *elseBlock = new mlir::Block();
    auto *mergeBlock = new mlir::Block();
    if (hasResult)
      mergeBlock->addArgument(convertType(ast->type), location);

    // Conditional branch from current block
    mlir::cf::CondBranchOp::create(builder, location, cond,
                                    thenBlock, /*trueArgs=*/{},
                                    elseBlock, /*falseArgs=*/{});

    // --- Then block ---
    parentRegion->push_back(thenBlock);
    builder.setInsertionPointToStart(thenBlock);
    auto thenVal = emitBlock(ast->thenBlockAST.get());

    auto *thenEnd = builder.getInsertionBlock();
    bool thenTerminated = !thenEnd->empty() &&
                          thenEnd->back().hasTrait<mlir::OpTrait::IsTerminator>();
    if (!thenTerminated) {
      if (hasResult)
        mlir::cf::BranchOp::create(builder, location, mergeBlock,
                                    mlir::ValueRange{thenVal});
      else
        mlir::cf::BranchOp::create(builder, location, mergeBlock);
    }

    // --- Else block ---
    parentRegion->push_back(elseBlock);
    builder.setInsertionPointToStart(elseBlock);
    auto elseVal = emitBlock(ast->elseBlockAST.get());

    auto *elseEnd = builder.getInsertionBlock();
    bool elseTerminated = !elseEnd->empty() &&
                          elseEnd->back().hasTrait<mlir::OpTrait::IsTerminator>();
    if (!elseTerminated) {
      if (hasResult)
        mlir::cf::BranchOp::create(builder, location, mergeBlock,
                                    mlir::ValueRange{elseVal});
      else
        mlir::cf::BranchOp::create(builder, location, mergeBlock);
    }

    // --- Merge block ---
    if (!thenTerminated || !elseTerminated) {
      parentRegion->push_back(mergeBlock);
      builder.setInsertionPointToStart(mergeBlock);
      return hasResult ? mergeBlock->getArgument(0) : nullptr;
    } else {
      // Both branches terminated (e.g. both returned) — merge unreachable
      delete mergeBlock;
      return nullptr;
    }
  }

  mlir::Value emitUnaryNegExpr(AST::UnaryNegExprAST *ast) {
    auto operand = emitExpr(ast->operand.get());
    if (!operand)
      return nullptr;

    auto location = loc(ast);
    const auto &type = ast->type;

    if (isIntegerType(type)) {
      auto zero = mlir::arith::ConstantIntOp::create(
          builder, location, 0,
          mlir::cast<mlir::IntegerType>(convertType(type)).getWidth());
      return mlir::arith::SubIOp::create(builder, location, zero, operand)
          .getResult();
    }

    if (isFloatType(type)) {
      return mlir::arith::NegFOp::create(builder, location, operand)
          .getResult();
    }

    sammine_util::abort("MLIRGen: unsupported unary negation type");
  }

  mlir::Value emitStringExpr(AST::StringExprAST *ast) {
    auto name = fmt::format(".str.{}", strCounter++);
    return getOrCreateGlobalString(name, ast->string_content, loc(ast));
  }

  // ===--- Phase 3: Pointer and array emission ---===

  mlir::Value emitArrayLiteralExpr(AST::ArrayLiteralExprAST *ast) {
    auto location = loc(ast);
    auto &arrType = std::get<ArrayType>(ast->type.type_data);
    auto elemMlirType = convertType(arrType.get_element());
    auto memrefType = mlir::MemRefType::get(
        {static_cast<int64_t>(arrType.get_size())}, elemMlirType);

    // Stack-allocate the array
    auto alloca = mlir::memref::AllocaOp::create(builder, location, memrefType);

    // Store each element
    for (size_t i = 0; i < ast->elements.size(); ++i) {
      auto elemVal = emitExpr(ast->elements[i].get());
      if (!elemVal)
        return nullptr;
      auto idx = mlir::arith::ConstantIndexOp::create(builder, location, i);
      mlir::memref::StoreOp::create(builder, location, elemVal, alloca,
                                     mlir::ValueRange{idx});
    }

    return alloca;
  }

  mlir::Value emitIndexExpr(AST::IndexExprAST *ast) {
    auto location = loc(ast);
    auto arr = emitExpr(ast->array_expr.get());
    auto idx = emitExpr(ast->index_expr.get());
    if (!arr || !idx)
      return nullptr;

    auto &arrType = std::get<ArrayType>(ast->array_expr->type.type_data);

    // Cast i32 index to index type for bounds check
    auto indexVal = mlir::arith::IndexCastOp::create(
        builder, location, builder.getIndexType(), idx);

    // Bounds check
    emitBoundsCheck(indexVal, arrType.get_size(), location);

    // If the array value is a raw !llvm.ptr (from dereferencing ptr<[T;N]>),
    // use LLVM GEP + load instead of memref.load.
    if (mlir::isa<mlir::LLVM::LLVMPointerType>(arr.getType())) {
      return emitPtrArrayLoad(arr, idx, arrType, location);
    }

    // Standard memref path
    return mlir::memref::LoadOp::create(builder, location, arr,
                                         mlir::ValueRange{indexVal});
  }

  /// Load element `idx` from array at raw pointer `ptr` using LLVM GEP.
  mlir::Value emitPtrArrayLoad(mlir::Value ptr, mlir::Value idx,
                                const ArrayType &arrType,
                                mlir::Location location) {
    auto elemMlirType = convertType(arrType.get_element());
    auto arrLLVMType = mlir::LLVM::LLVMArrayType::get(
        elemMlirType, arrType.get_size());
    auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());

    auto gepOp = mlir::LLVM::GEPOp::create(
        builder, location, ptrTy, arrLLVMType, ptr,
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, idx});
    return mlir::LLVM::LoadOp::create(builder, location, elemMlirType, gepOp);
  }

  /// Store `val` to element `idx` of array at raw pointer `ptr` using LLVM GEP.
  void emitPtrArrayStore(mlir::Value ptr, mlir::Value idx, mlir::Value val,
                          const ArrayType &arrType, mlir::Location location) {
    auto elemMlirType = convertType(arrType.get_element());
    auto arrLLVMType = mlir::LLVM::LLVMArrayType::get(
        elemMlirType, arrType.get_size());
    auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());

    auto gepOp = mlir::LLVM::GEPOp::create(
        builder, location, ptrTy, arrLLVMType, ptr,
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, idx});
    mlir::LLVM::StoreOp::create(builder, location, val, gepOp);
  }

  mlir::Value emitLenExpr(AST::LenExprAST *ast) {
    auto arrSize =
        std::get<ArrayType>(ast->operand->type.type_data).get_size();
    return mlir::arith::ConstantIntOp::create(builder, loc(ast),
                                              builder.getI32Type(),
                                              static_cast<int64_t>(arrSize))
        .getResult();
  }

  mlir::Value emitDerefExpr(AST::DerefExprAST *ast) {
    auto location = loc(ast);
    auto ptr = emitExpr(ast->operand.get());
    if (!ptr)
      return nullptr;

    auto &ptrType = std::get<PointerType>(ast->operand->type.type_data);
    auto pointeeType = ptrType.get_pointee();

    // Pointer-to-array: return the raw !llvm.ptr unchanged.
    // Downstream consumers (emitIndexExpr, assign-to-index) detect the
    // !llvm.ptr MLIR type and use LLVM GEP + load/store, so this works
    // regardless of how deeply nested the deref expression is.
    if (pointeeType.type_kind == TypeKind::Array)
      return ptr;

    auto mlirPointeeType = convertType(pointeeType);
    return mlir::LLVM::LoadOp::create(builder, location, mlirPointeeType, ptr);
  }

  mlir::Value emitAddrOfExpr(AST::AddrOfExprAST *ast) {
    auto *varExpr = dynamic_cast<AST::VariableExprAST *>(ast->operand.get());
    if (!varExpr)
      sammine_util::abort("MLIRGen: address-of (&) requires a variable operand");

    auto val = symbolTable.get_from_name(varExpr->variableName);

    // Arrays are still memref<NxT> — extract pointer from memref
    if (mlir::isa<mlir::MemRefType>(val.getType())) {
      auto location = loc(ast);
      auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
      auto idxVal =
          mlir::memref::ExtractAlignedPointerAsIndexOp::create(
              builder, location, val);
      auto i64Val = mlir::arith::IndexCastOp::create(
          builder, location, builder.getI64Type(), idxVal);
      return mlir::LLVM::IntToPtrOp::create(builder, location, ptrTy,
                                              i64Val.getResult(), nullptr);
    }

    // Non-array variables are in llvm.alloca — return the pointer directly
    return val;
  }

  mlir::Value emitAllocExpr(AST::AllocExprAST *ast) {
    auto location = loc(ast);
    auto countVal = emitExpr(ast->operand.get());
    if (!countVal)
      return nullptr;

    // Extract pointee type T from result type ptr<T>
    auto &ptrType = std::get<PointerType>(ast->type.type_data);
    int64_t elemSize = getTypeSize(ptrType.get_pointee());

    auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());
    auto i64Ty = builder.getI64Type();

    auto elemSizeVal = mlir::arith::ConstantIntOp::create(
        builder, location, i64Ty, elemSize);

    // Extend count to i64 if needed
    mlir::Value countI64 = countVal;
    if (countVal.getType() != i64Ty)
      countI64 = mlir::arith::ExtSIOp::create(builder, location, i64Ty,
                                                countVal);

    // total_size = sizeof(T) * count
    auto totalSize = mlir::arith::MulIOp::create(builder, location,
                                                   elemSizeVal, countI64);

    // Call malloc(total_size)
    auto mallocOp = mlir::LLVM::CallOp::create(
        builder, location, mlir::TypeRange{ptrTy}, "malloc",
        mlir::ValueRange{totalSize.getResult()});

    return mallocOp.getResult();
  }

  mlir::Value emitFreeExpr(AST::FreeExprAST *ast) {
    auto location = loc(ast);
    auto ptr = emitExpr(ast->operand.get());
    if (!ptr)
      return nullptr;

    mlir::LLVM::CallOp::create(builder, location, mlir::TypeRange{},
                                "free", mlir::ValueRange{ptr});

    return nullptr;
  }

  mlir::Value emitStructLiteralExpr(AST::StructLiteralExprAST *ast) {
    auto &st = std::get<StructType>(ast->type.type_data);
    auto structTy = structTypes.at(st.get_name());
    auto location = loc(ast);

    // Start with undef
    mlir::Value agg = mlir::LLVM::UndefOp::create(builder, location, structTy);

    // Insert each field value at its index
    for (size_t i = 0; i < ast->field_values.size(); i++) {
      auto fieldIdx = st.get_field_index(ast->field_names[i]);
      auto val = emitExpr(ast->field_values[i].get());
      agg = mlir::LLVM::InsertValueOp::create(builder, location, agg, val,
                                               fieldIdx.value());
    }
    return agg;
  }

  mlir::Value emitFieldAccessExpr(AST::FieldAccessExprAST *ast) {
    auto objVal = emitExpr(ast->object_expr.get());
    auto &st = std::get<StructType>(ast->object_expr->type.type_data);
    auto fieldIdx = st.get_field_index(ast->field_name);
    auto fieldType = convertType(st.get_field_type(fieldIdx.value()));
    return mlir::LLVM::ExtractValueOp::create(builder, loc(ast), fieldType,
                                               objVal, fieldIdx.value());
  }

  // ===--- Helpers ---===

  int64_t getTypeSize(const Type &type) {
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

  void emitBoundsCheck(mlir::Value idx, size_t arrSize, mlir::Location location) {
    auto i32Ty = builder.getI32Type();

    // Cast index to i32 for comparison
    auto idxI32 = mlir::arith::IndexCastOp::create(builder, location, i32Ty, idx);

    auto sizeConst = mlir::arith::ConstantIntOp::create(
        builder, location, i32Ty, static_cast<int64_t>(arrSize));
    auto zero = mlir::arith::ConstantIntOp::create(builder, location, i32Ty, 0);

    // idx >= size
    auto oobHigh = mlir::arith::CmpIOp::create(
        builder, location, mlir::arith::CmpIPredicate::sge, idxI32, sizeConst);
    // idx < 0
    auto oobLow = mlir::arith::CmpIOp::create(
        builder, location, mlir::arith::CmpIPredicate::slt, idxI32, zero);
    // oob = oobHigh || oobLow
    auto oob = mlir::arith::OrIOp::create(builder, location, oobHigh, oobLow);

    // scf.if (oob) { printf + exit(1) }
    auto ifOp = mlir::scf::IfOp::create(builder, location,
                                          /*resultTypes=*/mlir::TypeRange{},
                                          oob, /*withElseRegion=*/false);

    // The IfOp constructor auto-inserts a yield terminator in the then block.
    // Set insertion point before it so our ops go before the yield.
    auto &thenBlock = ifOp.getThenRegion().front();
    builder.setInsertionPoint(&thenBlock, thenBlock.begin());

    // exit(1) — uses func.call since exit is declared as func.func
    auto one = mlir::arith::ConstantIntOp::create(builder, location, i32Ty, 1);
    mlir::func::CallOp::create(builder, location, "exit",
                                mlir::TypeRange{}, mlir::ValueRange{one});

    builder.setInsertionPointAfter(ifOp);
  }

  /// Emit element-by-element array comparison using cf blocks as a loop.
  /// Returns i1: true if all elements equal (for ==), negated for !=.
  mlir::Value emitArrayComparison(mlir::Value lhs, mlir::Value rhs,
                                   const Type &arrType, TokenType tok,
                                   mlir::Location location) {
    auto &arrData = std::get<ArrayType>(arrType.type_data);
    int64_t size = static_cast<int64_t>(arrData.get_size());
    auto i1Ty = builder.getI1Type();
    auto indexTy = builder.getIndexType();

    auto *parentRegion = builder.getInsertionBlock()->getParent();

    // Create loop blocks: header, body, mismatch, exit
    auto *headerBlock = new mlir::Block();
    auto *bodyBlock = new mlir::Block();
    auto *mismatchBlock = new mlir::Block();
    auto *exitBlock = new mlir::Block();

    // header takes a loop counter (index type)
    headerBlock->addArgument(indexTy, location);
    // exit takes the result (i1)
    exitBlock->addArgument(i1Ty, location);

    // Branch from current block to header with counter = 0
    auto zero = mlir::arith::ConstantIndexOp::create(builder, location, 0);
    mlir::cf::BranchOp::create(builder, location, headerBlock,
                                mlir::ValueRange{zero});

    // --- Header block: check i < size ---
    parentRegion->push_back(headerBlock);
    builder.setInsertionPointToStart(headerBlock);
    auto counter = headerBlock->getArgument(0);
    auto sizeVal = mlir::arith::ConstantIndexOp::create(builder, location, size);
    auto cond = mlir::arith::CmpIOp::create(
        builder, location, mlir::arith::CmpIPredicate::slt, counter, sizeVal);
    // If i < size → body, else → exit(true = all matched)
    auto trueVal = mlir::arith::ConstantIntOp::create(builder, location, i1Ty, 1);
    mlir::cf::CondBranchOp::create(builder, location, cond,
                                    bodyBlock, {},
                                    exitBlock, mlir::ValueRange{trueVal});

    // --- Body block: compare elements[i] ---
    parentRegion->push_back(bodyBlock);
    builder.setInsertionPointToStart(bodyBlock);
    auto lhsElem = mlir::memref::LoadOp::create(builder, location, lhs,
                                                  mlir::ValueRange{counter});
    auto rhsElem = mlir::memref::LoadOp::create(builder, location, rhs,
                                                  mlir::ValueRange{counter});
    // Compare elements
    mlir::Value elemEq;
    if (isIntegerType(arrData.get_element()) || isBoolType(arrData.get_element()))
      elemEq = mlir::arith::CmpIOp::create(
          builder, location, mlir::arith::CmpIPredicate::eq, lhsElem, rhsElem);
    else if (isFloatType(arrData.get_element()))
      elemEq = mlir::arith::CmpFOp::create(
          builder, location, mlir::arith::CmpFPredicate::OEQ, lhsElem, rhsElem);
    else
      sammine_util::abort("MLIRGen: unsupported array element type for comparison");

    // If equal → increment and loop back to header; else → mismatch
    auto one = mlir::arith::ConstantIndexOp::create(builder, location, 1);
    auto next = mlir::arith::AddIOp::create(builder, location, counter, one);
    mlir::cf::CondBranchOp::create(builder, location, elemEq,
                                    headerBlock, mlir::ValueRange{next.getResult()},
                                    mismatchBlock, {});

    // --- Mismatch block: exit with false ---
    parentRegion->push_back(mismatchBlock);
    builder.setInsertionPointToStart(mismatchBlock);
    auto falseVal = mlir::arith::ConstantIntOp::create(builder, location, i1Ty, 0);
    mlir::cf::BranchOp::create(builder, location, exitBlock,
                                mlir::ValueRange{falseVal});

    // --- Exit block: result via block argument ---
    parentRegion->push_back(exitBlock);
    builder.setInsertionPointToStart(exitBlock);
    auto result = exitBlock->getArgument(0);

    // For !=, negate the result
    if (tok == TokNOTEqual) {
      auto trueConst = mlir::arith::ConstantIntOp::create(builder, location, i1Ty, 1);
      return mlir::arith::XOrIOp::create(builder, location, result, trueConst)
          .getResult();
    }
    return result;
  }

  mlir::Value getOrCreateGlobalString(llvm::StringRef name,
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
};

} // anonymous namespace

mlir::OwningOpRef<mlir::ModuleOp>
mlirGen(mlir::MLIRContext &context, AST::ProgramAST *program,
        const std::string &moduleName) {
  return MLIRGenImpl(context, moduleName).generate(program);
}

} // namespace sammine_lang
