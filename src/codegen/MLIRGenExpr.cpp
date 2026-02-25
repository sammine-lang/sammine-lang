#include "codegen/MLIRGenImpl.h"

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

#include "llvm/ADT/SmallVector.h"

#include "fmt/core.h"

namespace sammine_lang {

mlir::Value MLIRGenImpl::emitNumberExpr(AST::NumberExprAST *ast) {
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

mlir::Value MLIRGenImpl::emitBoolExpr(AST::BoolExprAST *ast) {
  return mlir::arith::ConstantIntOp::create(builder, loc(ast),
                                            ast->b ? 1 : 0, 1)
      .getResult();
}

mlir::Value MLIRGenImpl::emitCharExpr(AST::CharExprAST *ast) {
  return mlir::arith::ConstantIntOp::create(
             builder, loc(ast), static_cast<uint8_t>(ast->value), 8)
      .getResult();
}

mlir::Value MLIRGenImpl::emitUnitExpr(AST::UnitExprAST *) {
  // Unit has no runtime representation. Return nullptr — callers
  // that need a value should handle this.
  return nullptr;
}

mlir::Value MLIRGenImpl::emitVariableExpr(AST::VariableExprAST *ast) {
  auto location = loc(ast);

  // If the variable is in the symbol table, handle it normally
  if (symbolTable.queryName(ast->variableName) != nameNotFound) {
    auto val = symbolTable.get_from_name(ast->variableName);

    // Array (memref<NxT>): return the memref itself
    if (mlir::isa<mlir::MemRefType>(val.getType()))
      return val;

    // Variable in llvm.alloca: load the value
    if (mlir::isa<mlir::LLVM::LLVMPointerType>(val.getType())) {
      auto elemType = convertType(ast->type);
      return mlir::LLVM::LoadOp::create(builder, location, elemType, val);
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
      auto ptrTy = llvmPtrTy();
      auto wrapperAddr = mlir::LLVM::AddressOfOp::create(
          builder, location, ptrTy, wrapperName);
      auto nullEnv = mlir::LLVM::ZeroOp::create(builder, location, ptrTy);
      return buildClosure(wrapperAddr, nullEnv, location);
    }
  }

  sammine_util::abort(
      fmt::format("MLIRGen: unknown variable '{}'", ast->variableName));
}

mlir::Value MLIRGenImpl::emitBinaryExpr(AST::BinaryExprAST *ast) {
  auto location = loc(ast);

  // Assignment: store RHS into LHS's memref
  if (ast->Op->is_assign()) {
    // *p = val
    if (auto *deref = dynamic_cast<AST::DerefExprAST *>(ast->LHS.get())) {
      auto ptr = emitExpr(deref->operand.get());
      auto rhs = emitExpr(ast->RHS.get());
      if (!ptr || !rhs)
        return nullptr;
      mlir::LLVM::StoreOp::create(builder, location, rhs, ptr);
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
        emitPtrArrayStore(arr, idx, rhs, arrType, location);
        return rhs;
      }

      auto indexVal = mlir::arith::IndexCastOp::create(
          builder, location, builder.getIndexType(), idx);
      mlir::memref::StoreOp::create(builder, location, rhs, arr,
                                     mlir::ValueRange{indexVal});
      return rhs;
    }

    // x = val (mutable scalar)
    mlir::Value rhs = emitExpr(ast->RHS.get());
    if (!rhs)
      return nullptr;

    if (auto *lhsVar = dynamic_cast<AST::VariableExprAST *>(ast->LHS.get())) {
      auto varPtr = symbolTable.get_from_name(lhsVar->variableName);
      mlir::LLVM::StoreOp::create(builder, location, rhs, varPtr);
      return rhs;
    }

    sammine_util::abort("MLIRGen: unsupported assignment LHS");
  }

  mlir::Value lhs = emitExpr(ast->LHS.get());
  mlir::Value rhs = emitExpr(ast->RHS.get());
  if (!lhs || !rhs)
    return nullptr;
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
            "MLIRGen: unsupported integer comparison operator '{}'",
            ast->Op->lexeme));
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
            "MLIRGen: unsupported float comparison operator '{}'",
            ast->Op->lexeme));
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

mlir::Value MLIRGenImpl::emitUnaryNegExpr(AST::UnaryNegExprAST *ast) {
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

mlir::Value MLIRGenImpl::emitIfExpr(AST::IfExprAST *ast) {
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

mlir::Value MLIRGenImpl::emitStringExpr(AST::StringExprAST *ast) {
  auto name = fmt::format("{}{}", kStringPrefix, strCounter++);
  return getOrCreateGlobalString(name, ast->string_content, loc(ast));
}

// ===--- Array emission ---===

mlir::Value
MLIRGenImpl::emitArrayLiteralExpr(AST::ArrayLiteralExprAST *ast) {
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

mlir::Value MLIRGenImpl::emitIndexExpr(AST::IndexExprAST *ast) {
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

mlir::Value MLIRGenImpl::emitPtrArrayGEP(mlir::Value ptr, mlir::Value idx,
                                          const ArrayType &arrType,
                                          mlir::Location location) {
  auto elemMlirType = convertType(arrType.get_element());
  auto arrLLVMType =
      mlir::LLVM::LLVMArrayType::get(elemMlirType, arrType.get_size());
  return mlir::LLVM::GEPOp::create(builder, location, llvmPtrTy(),
                                     arrLLVMType, ptr,
                                     llvm::ArrayRef<mlir::LLVM::GEPArg>{0, idx});
}

mlir::Value MLIRGenImpl::emitPtrArrayLoad(mlir::Value ptr, mlir::Value idx,
                                           const ArrayType &arrType,
                                           mlir::Location location) {
  auto gepOp = emitPtrArrayGEP(ptr, idx, arrType, location);
  return mlir::LLVM::LoadOp::create(builder, location,
                                     convertType(arrType.get_element()), gepOp);
}

void MLIRGenImpl::emitPtrArrayStore(mlir::Value ptr, mlir::Value idx,
                                     mlir::Value val,
                                     const ArrayType &arrType,
                                     mlir::Location location) {
  auto gepOp = emitPtrArrayGEP(ptr, idx, arrType, location);
  mlir::LLVM::StoreOp::create(builder, location, val, gepOp);
}

mlir::Value MLIRGenImpl::emitLenExpr(AST::LenExprAST *ast) {
  auto arrSize =
      std::get<ArrayType>(ast->operand->type.type_data).get_size();
  return mlir::arith::ConstantIntOp::create(builder, loc(ast),
                                            builder.getI32Type(),
                                            static_cast<int64_t>(arrSize))
      .getResult();
}

// ===--- Pointer emission ---===

mlir::Value MLIRGenImpl::emitDerefExpr(AST::DerefExprAST *ast) {
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

mlir::Value MLIRGenImpl::emitAddrOfExpr(AST::AddrOfExprAST *ast) {
  auto *varExpr = dynamic_cast<AST::VariableExprAST *>(ast->operand.get());
  if (!varExpr)
    sammine_util::abort("MLIRGen: address-of (&) requires a variable operand");

  auto val = symbolTable.get_from_name(varExpr->variableName);

  // Arrays are still memref<NxT> — extract pointer from memref
  if (mlir::isa<mlir::MemRefType>(val.getType())) {
    auto location = loc(ast);
    auto ptrTy = llvmPtrTy();
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

mlir::Value MLIRGenImpl::emitAllocExpr(AST::AllocExprAST *ast) {
  auto location = loc(ast);
  auto countVal = emitExpr(ast->operand.get());
  if (!countVal)
    return nullptr;

  // Extract pointee type T from result type ptr<T>
  auto &ptrType = std::get<PointerType>(ast->type.type_data);
  int64_t elemSize = getTypeSize(ptrType.get_pointee());

  auto ptrTy = llvmPtrTy();
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
      builder, location, mlir::TypeRange{ptrTy}, kMallocFunc,
      mlir::ValueRange{totalSize.getResult()});

  return mallocOp.getResult();
}

mlir::Value MLIRGenImpl::emitFreeExpr(AST::FreeExprAST *ast) {
  auto location = loc(ast);
  auto ptr = emitExpr(ast->operand.get());
  if (!ptr)
    return nullptr;

  mlir::LLVM::CallOp::create(builder, location, mlir::TypeRange{},
                              kFreeFunc, mlir::ValueRange{ptr});

  return nullptr;
}

// ===--- Struct emission ---===

mlir::Value
MLIRGenImpl::emitStructLiteralExpr(AST::StructLiteralExprAST *ast) {
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

mlir::Value
MLIRGenImpl::emitFieldAccessExpr(AST::FieldAccessExprAST *ast) {
  auto objVal = emitExpr(ast->object_expr.get());
  auto &st = std::get<StructType>(ast->object_expr->type.type_data);
  auto fieldIdx = st.get_field_index(ast->field_name);
  auto fieldType = convertType(st.get_field_type(fieldIdx.value()));
  return mlir::LLVM::ExtractValueOp::create(builder, loc(ast), fieldType,
                                             objVal, fieldIdx.value());
}

// ===--- Bounds check and array comparison ---===

void MLIRGenImpl::emitBoundsCheck(mlir::Value idx, size_t arrSize,
                                   mlir::Location location) {
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
  mlir::func::CallOp::create(builder, location, kExitFunc,
                              mlir::TypeRange{}, mlir::ValueRange{one});

  builder.setInsertionPointAfter(ifOp);
}

mlir::Value MLIRGenImpl::emitArrayComparison(mlir::Value lhs, mlir::Value rhs,
                                              const Type &arrType,
                                              TokenType tok,
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

} // namespace sammine_lang
