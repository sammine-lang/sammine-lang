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
    case TypeKind::Function: {
      auto &funcType = std::get<FunctionType>(type.type_data);
      llvm::SmallVector<mlir::Type, 4> paramTypes;
      for (auto &p : funcType.get_params_types())
        paramTypes.push_back(convertType(p));
      llvm::SmallVector<mlir::Type, 1> retTypes;
      auto retType = funcType.get_return_type();
      if (retType.type_kind != TypeKind::Unit)
        retTypes.push_back(convertType(retType));
      return builder.getFunctionType(paramTypes, retTypes);
    }
    case TypeKind::Array: {
      auto &arrType = std::get<ArrayType>(type.type_data);
      return mlir::MemRefType::get({static_cast<int64_t>(arrType.get_size())},
                                   convertType(arrType.get_element()));
    }
    case TypeKind::Struct:
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
      ; // Phase 4: structs
    else if (dynamic_cast<AST::TypeClassDeclAST *>(def))
      ; // Skip typeclass declarations
    else if (dynamic_cast<AST::TypeClassInstanceAST *>(def))
      ; // Skip typeclass instances for now
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
      if (retType.type_kind != TypeKind::Unit)
        retTypes.push_back(convertType(retType));
    }

    return builder.getFunctionType(argTypes, retTypes);
  }

  std::string mangleName(const std::string &name) {
    if (!moduleName.empty())
      return moduleName + "$" + name;
    return name;
  }

  void emitFunction(AST::FuncDefAST *ast) {
    // Skip generic function templates — only monomorphized copies
    if (ast->Prototype->is_generic())
      return;

    symbolTable.push_context();

    auto funcType = buildFuncType(ast->Prototype.get());
    std::string funcName = mangleName(ast->Prototype->functionName);

    builder.setInsertionPointToEnd(theModule.getBody());
    auto funcOp = mlir::func::FuncOp::create(
        loc(ast), llvm::StringRef(funcName), funcType);
    theModule.push_back(funcOp);

    // Create entry block with arguments
    auto &entryBlock = *funcOp.addEntryBlock();

    // Bind parameters in symbol table
    for (size_t i = 0; i < ast->Prototype->parameterVectors.size(); ++i) {
      symbolTable.registerNameT(ast->Prototype->parameterVectors[i]->name,
                                entryBlock.getArgument(i));
    }

    builder.setInsertionPointToStart(&entryBlock);

    // Emit the function body
    mlir::Value bodyResult = emitBlock(ast->Block.get());

    // If the block didn't end with a return, insert one.
    // Check if the current block already has a terminator.
    auto *currentBlock = builder.getInsertionBlock();
    if (currentBlock->empty() ||
        !currentBlock->back().hasTrait<mlir::OpTrait::IsTerminator>()) {
      if (funcType.getNumResults() == 0) {
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

    symbolTable.pop_context();
  }

  void emitExtern(AST::ExternAST *ast) {
    std::string funcName = ast->Prototype->functionName;

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
    if (symbolTable.recursiveQueryName(ast->variableName) == nameNotFound)
      sammine_util::abort(
          fmt::format("MLIRGen: unknown variable '{}'", ast->variableName));
    auto val = symbolTable.recursive_get_from_name(ast->variableName);

    if (auto memrefTy = mlir::dyn_cast<mlir::MemRefType>(val.getType())) {
      // Array (rank > 0): return the memref itself — it IS the value
      if (memrefTy.getRank() > 0)
        return val;
      // Mutable scalar (rank 0): load the value
      return mlir::memref::LoadOp::create(builder, loc(ast), val,
                                           mlir::ValueRange{});
    }
    return val;
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

      // arr[i] = val
      if (auto *idxExpr =
              dynamic_cast<AST::IndexExprAST *>(ast->LHS.get())) {
        auto arr = emitExpr(idxExpr->array_expr.get());
        auto idx = emitExpr(idxExpr->index_expr.get());
        auto rhs = emitExpr(ast->RHS.get());
        if (!arr || !idx || !rhs)
          return nullptr;
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
        auto memref =
            symbolTable.recursive_get_from_name(lhsVar->variableName);
        mlir::memref::StoreOp::create(builder, loc(ast), rhs, memref,
                                       mlir::ValueRange{});
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

  mlir::Value emitCallExpr(AST::CallExprAST *ast) {
    auto location = loc(ast);

    // Resolve callee name — use resolved_generic_name if set by monomorphizer
    std::string callee;
    if (ast->resolved_generic_name.has_value())
      callee = mangleName(*ast->resolved_generic_name);
    else
      callee = mangleName(ast->functionName.mangled());

    // If the mangled callee isn't declared, fall back to the base name.
    // This handles imported C externs: call site uses "std$printf" but
    // the extern is declared as "printf".
    if (!theModule.lookupSymbol(callee)) {
      auto baseName = ast->functionName.name;
      if (theModule.lookupSymbol(baseName))
        callee = baseName;
    }

    // Emit arguments
    llvm::SmallVector<mlir::Value, 4> operands;
    for (auto &arg : ast->arguments) {
      auto val = emitExpr(arg.get());
      if (val)
        operands.push_back(val);
    }

    // Determine result types
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

  mlir::Value emitReturnExpr(AST::ReturnExprAST *ast) {
    auto location = loc(ast);

    if (ast->return_expr) {
      auto val = emitExpr(ast->return_expr.get());
      if (val)
        mlir::func::ReturnOp::create(builder, location,
                                     mlir::ValueRange{val});
      else
        mlir::func::ReturnOp::create(builder, location);
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

    if (ast->is_mutable) {
      // Mutable var: alloca + store, register the memref
      auto elemType = convertType(ast->type);
      auto memrefType = mlir::MemRefType::get({}, elemType);
      auto location = loc(ast);
      auto alloca =
          mlir::memref::AllocaOp::create(builder, location, memrefType);
      mlir::memref::StoreOp::create(builder, location, initVal, alloca,
                                     mlir::ValueRange{});
      symbolTable.registerNameT(ast->TypedVar->name, alloca);
    } else {
      // Immutable let: SSA value directly
      symbolTable.registerNameT(ast->TypedVar->name, initVal);
    }
    return initVal;
  }

  mlir::Value emitIfExpr(AST::IfExprAST *ast) {
    auto location = loc(ast);
    auto cond = emitExpr(ast->bool_expr.get());
    if (!cond)
      return nullptr;

    bool hasResult = ast->type.type_kind != TypeKind::Unit &&
                     ast->type.type_kind != TypeKind::Never;

    mlir::SmallVector<mlir::Type, 1> resultTypes;
    if (hasResult)
      resultTypes.push_back(convertType(ast->type));

    auto ifOp = mlir::scf::IfOp::create(builder, location, resultTypes, cond,
                                         /*withElseRegion=*/true);

    // Emit then region
    builder.setInsertionPointToStart(&ifOp.getThenRegion().front());
    auto thenVal = emitBlock(ast->thenBlockAST.get());
    if (hasResult)
      mlir::scf::YieldOp::create(builder, location, mlir::ValueRange{thenVal});
    else
      mlir::scf::YieldOp::create(builder, location);

    // Emit else region
    builder.setInsertionPointToStart(&ifOp.getElseRegion().front());
    auto elseVal = emitBlock(ast->elseBlockAST.get());
    if (hasResult)
      mlir::scf::YieldOp::create(builder, location, mlir::ValueRange{elseVal});
    else
      mlir::scf::YieldOp::create(builder, location);

    // Restore insertion point after the if op
    builder.setInsertionPointAfter(ifOp);

    return hasResult ? ifOp.getResult(0) : nullptr;
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

    // Cast i32 index to index type
    auto indexVal = mlir::arith::IndexCastOp::create(
        builder, location, builder.getIndexType(), idx);

    // Bounds check
    auto &arrType = std::get<ArrayType>(ast->array_expr->type.type_data);
    emitBoundsCheck(indexVal, arrType.get_size(), location);

    // Load element
    return mlir::memref::LoadOp::create(builder, location, arr,
                                         mlir::ValueRange{indexVal});
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

    if (pointeeType.type_kind == TypeKind::Array)
      sammine_util::abort("MLIRGen: deref of pointer-to-array not yet supported");

    auto mlirPointeeType = convertType(pointeeType);
    return mlir::LLVM::LoadOp::create(builder, location, mlirPointeeType, ptr);
  }

  mlir::Value emitAddrOfExpr(AST::AddrOfExprAST *ast) {
    auto location = loc(ast);
    auto *varExpr = dynamic_cast<AST::VariableExprAST *>(ast->operand.get());
    if (!varExpr)
      sammine_util::abort("MLIRGen: address-of (&) requires a variable operand");

    auto val = symbolTable.recursive_get_from_name(varExpr->variableName);
    auto ptrTy = mlir::LLVM::LLVMPointerType::get(builder.getContext());

    if (auto memrefTy = mlir::dyn_cast<mlir::MemRefType>(val.getType())) {
      // Mutable var or array: extract pointer from memref
      auto idxVal =
          mlir::memref::ExtractAlignedPointerAsIndexOp::create(
              builder, location, val);
      auto i64Val = mlir::arith::IndexCastOp::create(
          builder, location, builder.getI64Type(), idxVal);
      return mlir::LLVM::IntToPtrOp::create(builder, location, ptrTy,
                                              i64Val.getResult(), nullptr);
    }

    // Immutable SSA value: spill to stack via llvm.alloca, store, return ptr
    auto valType = val.getType();
    auto one = mlir::arith::ConstantIntOp::create(builder, location,
                                                   builder.getI64Type(), 1);
    auto alloca =
        mlir::LLVM::AllocaOp::create(builder, location, ptrTy, valType, one);
    mlir::LLVM::StoreOp::create(builder, location, val, alloca);
    return alloca;
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
