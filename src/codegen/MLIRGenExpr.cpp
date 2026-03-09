#include "ast/Ast.h"
#include "codegen/MLIRGenImpl.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinTypes.h"

#include "llvm/ADT/SmallVector.h"
#include <mlir/IR/Block.h>

#include "fmt/core.h"

namespace sammine_lang {

mlir::Value MLIRGenImpl::emitNumberExpr(AST::NumberExprAST *ast) {
  auto location = loc(ast);
  auto type = ast->get_type();

  if (isFloatType(type)) {
    auto mlirType = mlir::cast<mlir::FloatType>(convertType(type));
    double val = std::stod(ast->number);
    llvm::APFloat apVal(val);
    if (type.type_kind == TypeKind::F32_t) {
      bool losesInfo = false;
      apVal.convert(llvm::APFloat::IEEEsingle(),
                    llvm::APFloat::rmNearestTiesToEven, &losesInfo);
    }
    return mlir::arith::ConstantFloatOp::create(builder, location, mlirType,
                                                apVal)
        .getResult();
  }

  // Integer
  auto mlirType = convertType(type);
  int64_t val = std::stoll(ast->number);
  return mlir::arith::ConstantIntOp::create(builder, location, mlirType, val)
      .getResult();
}

mlir::Value MLIRGenImpl::emitBoolExpr(AST::BoolExprAST *ast) {
  return mlir::arith::ConstantIntOp::create(builder, loc(ast), ast->b ? 1 : 0,
                                            1)
      .getResult();
}

mlir::Value MLIRGenImpl::emitCharExpr(AST::CharExprAST *ast) {
  return mlir::arith::ConstantIntOp::create(builder, loc(ast),
                                            static_cast<uint8_t>(ast->value), 8)
      .getResult();
}

mlir::Value MLIRGenImpl::emitUnitExpr(AST::UnitExprAST *) {
  // Unit has no runtime representation. Return nullptr — callers
  // that need a value should handle this.
  return nullptr;
}

mlir::Value MLIRGenImpl::emitVariableExpr(AST::VariableExprAST *ast) {
  auto location = loc(ast);
  auto *vp = props_.variable(ast->id());

  // Enum unit variant
  if (vp && vp->is_enum_unit_variant) {
    auto et = std::get<EnumType>(ast->get_type().type_data);

    // Integer-backed enum: emit bare integer constant
    if (et.is_integer_backed()) {
      auto &vi = et.get_variant(vp->enum_variant_index);
      return mlir::arith::ConstantIntOp::create(builder, location,
                                                getEnumBackingMLIRType(et),
                                                vi.discriminant_value.value())
          .getResult();
    }

    // Tagged union: build { tag, undef_payload }
    auto enumTy = enumTypes.at(et.get_name().mangled());

    auto alloca = emitAllocaOne(enumTy, location);

    auto tagVal = mlir::arith::ConstantIntOp::create(
        builder, location, builder.getI32Type(),
        static_cast<int64_t>(vp->enum_variant_index));
    auto tagPtr = mlir::LLVM::GEPOp::create(
        builder, location, llvmPtrTy(), enumTy, alloca,
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, 0},
        mlir::LLVM::GEPNoWrapFlags::inbounds);
    mlir::LLVM::StoreOp::create(builder, location, tagVal, tagPtr);

    return mlir::LLVM::LoadOp::create(builder, location, enumTy, alloca);
  }

  // If the variable is in the symbol table, handle it normally
  if (symbolTable.queryName(ast->variableName) != nameNotFound) {
    auto val = symbolTable.get_from_name(ast->variableName);

    // Array vars are !llvm.ptr (alloca to LLVMArrayType) — return the
    // pointer directly; downstream consumers use GEP to index into it.
    if (ast->get_type().type_kind == TypeKind::Array &&
        mlir::isa<mlir::LLVM::LLVMPointerType>(val.getType()))
      return val;

    // Variable in llvm.alloca: load the value
    if (mlir::isa<mlir::LLVM::LLVMPointerType>(val.getType())) {
      auto elemType = convertType(ast->get_type());
      return mlir::LLVM::LoadOp::create(builder, location, elemType, val);
    }

    return val;
  }

  // Not in symbol table — check if it's a module-level function used as a
  // value. VariableExprAST stores a bare name (e.g. "foo"), but top-level
  // functions may be registered under their module-qualified name (e.g.
  // "mylib::foo"). Try both the bare name and the module-qualified name.
  if (ast->get_type().type_kind == TypeKind::Function) {
    auto funcName = ast->variableName;
    if (!theModule.lookupSymbol<mlir::func::FuncOp>(funcName) &&
        !moduleName.empty())
      funcName = moduleName + "::" + funcName;
    if (theModule.lookupSymbol<mlir::func::FuncOp>(funcName)) {
      auto ft = std::get<FunctionType>(ast->get_type().type_data);
      auto wrapperName = getOrCreateClosureWrapper(funcName, ft);
      auto ptrTy = llvmPtrTy();
      auto wrapperAddr = mlir::LLVM::AddressOfOp::create(builder, location,
                                                         ptrTy, wrapperName);
      auto nullEnv = mlir::LLVM::ZeroOp::create(builder, location, ptrTy);
      return buildClosure(wrapperAddr, nullEnv, location);
    }
  }

  imm_error(fmt::format("unknown variable '{}'", ast->variableName),
            ast->get_location());
}

mlir::Value MLIRGenImpl::emitBinaryExpr(AST::BinaryExprAST *ast) {
  auto location = loc(ast);

  // Assignment: store RHS into LHS's address
  if (ast->Op->is_assign()) {
    auto ptr = emitLValue(ast->LHS.get());
    auto rhs = emitRValue(ast->RHS.get());
    if (!ptr || !rhs)
      return nullptr;
    mlir::LLVM::StoreOp::create(builder, location, rhs, ptr);
    return rhs;
  }

  mlir::Value lhs = emitExpr(ast->LHS.get());
  mlir::Value rhs = emitExpr(ast->RHS.get());
  if (!lhs || !rhs)
    return nullptr;
  auto resultType = ast->get_type();

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
      if (isUnsignedIntegerType(resultType))
        return mlir::arith::DivUIOp::create(builder, location, lhs, rhs)
            .getResult();
      return mlir::arith::DivSIOp::create(builder, location, lhs, rhs)
          .getResult();
    case TokMOD:
      if (isUnsignedIntegerType(resultType))
        return mlir::arith::RemUIOp::create(builder, location, lhs, rhs)
            .getResult();
      return mlir::arith::RemSIOp::create(builder, location, lhs, rhs)
          .getResult();
    case TokAndLogical:
      return mlir::arith::AndIOp::create(builder, location, lhs, rhs)
          .getResult();
    case TokORLogical:
      return mlir::arith::OrIOp::create(builder, location, lhs, rhs)
          .getResult();
    case TokXOR:
      return mlir::arith::XOrIOp::create(builder, location, lhs, rhs)
          .getResult();
    case TokSHL:
      return mlir::arith::ShLIOp::create(builder, location, lhs, rhs)
          .getResult();
    case TokSHR:
      if (isUnsignedIntegerType(resultType))
        return mlir::arith::ShRUIOp::create(builder, location, lhs, rhs)
            .getResult();
      return mlir::arith::ShRSIOp::create(builder, location, lhs, rhs)
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
    auto lhsType = ast->LHS->get_type();

    if (isIntegerType(lhsType)) {
      bool is_unsigned = isUnsignedIntegerType(lhsType);
      mlir::arith::CmpIPredicate pred;
      switch (ast->Op->tok_type) {
      case TokEQUAL:
        pred = mlir::arith::CmpIPredicate::eq;
        break;
      case TokNOTEqual:
        pred = mlir::arith::CmpIPredicate::ne;
        break;
      case TokLESS:
        pred = is_unsigned ? mlir::arith::CmpIPredicate::ult
                           : mlir::arith::CmpIPredicate::slt;
        break;
      case TokLessEqual:
        pred = is_unsigned ? mlir::arith::CmpIPredicate::ule
                           : mlir::arith::CmpIPredicate::sle;
        break;
      case TokGREATER:
        pred = is_unsigned ? mlir::arith::CmpIPredicate::ugt
                           : mlir::arith::CmpIPredicate::sgt;
        break;
      case TokGreaterEqual:
        pred = is_unsigned ? mlir::arith::CmpIPredicate::uge
                           : mlir::arith::CmpIPredicate::sge;
        break;
      case TokAND:
        return mlir::arith::AndIOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokOR:
        return mlir::arith::OrIOp::create(builder, location, lhs, rhs)
            .getResult();
      default:
        imm_error(
            fmt::format("unsupported integer operator '{}'", ast->Op->lexeme),
            ast->get_location());
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
        imm_error(
            fmt::format("unsupported float operator '{}'", ast->Op->lexeme),
            ast->get_location());
      }
      return mlir::arith::CmpFOp::create(builder, location, pred, lhs, rhs)
          .getResult();
    }

    // Pointer comparisons (== and !=)
    if (lhsType.type_kind == TypeKind::Pointer) {
      auto i64Ty = builder.getI64Type();
      auto lhsInt =
          mlir::LLVM::PtrToIntOp::create(builder, location, i64Ty, lhs);
      auto rhsInt =
          mlir::LLVM::PtrToIntOp::create(builder, location, i64Ty, rhs);
      mlir::arith::CmpIPredicate pred;
      switch (ast->Op->tok_type) {
      case TokEQUAL:
        pred = mlir::arith::CmpIPredicate::eq;
        break;
      case TokNOTEqual:
        pred = mlir::arith::CmpIPredicate::ne;
        break;
      default:
        imm_error("only == and != supported for pointers", ast->get_location());
      }
      return mlir::arith::CmpIOp::create(builder, location, pred, lhsInt,
                                         rhsInt)
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
        imm_error(
            fmt::format("unsupported bool operator '{}'", ast->Op->lexeme),
            ast->get_location());
      }
      return mlir::arith::CmpIOp::create(builder, location, pred, lhs, rhs)
          .getResult();
    }
  }

  // Integer-backed enum bitwise ops: values are bare integers
  if (resultType.type_kind == TypeKind::Enum) {
    auto &et = std::get<EnumType>(resultType.type_data);
    if (et.is_integer_backed() && ast->Op->is_bitwise()) {
      switch (ast->Op->tok_type) {
      case TokAndLogical:
        return mlir::arith::AndIOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokORLogical:
        return mlir::arith::OrIOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokXOR:
        return mlir::arith::XOrIOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokSHL:
        return mlir::arith::ShLIOp::create(builder, location, lhs, rhs)
            .getResult();
      case TokSHR: {
        auto bt = et.get_backing_type();
        if (bt == TypeKind::U32_t || bt == TypeKind::U64_t)
          return mlir::arith::ShRUIOp::create(builder, location, lhs, rhs)
              .getResult();
        return mlir::arith::ShRSIOp::create(builder, location, lhs, rhs)
            .getResult();
      }
      default:
        break;
      }
    }
  }

  // User-defined operator via typeclass instance — emit a function call
  auto *bp = props_.binary(ast->id());
  if (bp && bp->resolved_op_method.has_value()) {
    auto opName = bp->resolved_op_method->mangled();
    auto funcOp = theModule.lookupSymbol<mlir::func::FuncOp>(opName);
    if (!funcOp)
      funcOp = theModule.lookupSymbol<mlir::func::FuncOp>(opName);
    if (funcOp) {
      auto callOp = mlir::func::CallOp::create(builder, location, funcOp,
                                               mlir::ValueRange{lhs, rhs});
      return callOp.getResult(0);
    }
  }

  imm_error(fmt::format("unsupported binary operator '{}'", ast->Op->lexeme),
            ast->get_location());
}

mlir::Value MLIRGenImpl::emitUnaryNegExpr(AST::UnaryNegExprAST *ast) {
  auto operand = emitExpr(ast->operand.get());
  if (!operand)
    return nullptr;

  auto location = loc(ast);
  auto type = ast->get_type();

  if (isIntegerType(type)) {
    auto zero = mlir::arith::ConstantIntOp::create(
        builder, location, 0,
        mlir::cast<mlir::IntegerType>(convertType(type)).getWidth());
    return mlir::arith::SubIOp::create(builder, location, zero, operand)
        .getResult();
  }

  if (isFloatType(type)) {
    return mlir::arith::NegFOp::create(builder, location, operand).getResult();
  }

  imm_error(
      fmt::format("unsupported unary negation on type '{}'", type.to_string()),
      ast->get_location());
}

mlir::Value MLIRGenImpl::emitIfExpr(AST::IfExprAST *ast) {
  auto location = loc(ast);
  auto cond = emitExpr(ast->bool_expr.get());
  if (!cond)
    return nullptr;

  bool hasResult = ast->get_type().type_kind != TypeKind::Unit &&
                   ast->get_type().type_kind != TypeKind::Never;

  auto *parentRegion = builder.getInsertionBlock()->getParent();

  auto *thenBlock = addBlock(parentRegion);
  auto *elseBlock = addBlock(parentRegion);
  auto *mergeBlock = new mlir::Block();
  if (hasResult) {
    // Array expressions produce !llvm.ptr (alloca pointers), not LLVMArrayType
    auto mergeType = ast->get_type().type_kind == TypeKind::Array
                         ? mlir::Type(llvmPtrTy())
                         : convertType(ast->get_type());
    mergeBlock->addArgument(mergeType, location);
  }

  // Conditional branch from current block
  mlir::cf::CondBranchOp::create(builder, location, cond, thenBlock,
                                 /*trueArgs=*/{}, elseBlock, /*falseArgs=*/{});

  auto conf_helper = [this, hasResult, location,
                      mergeBlock](mlir::Block *b, AST::BlockAST *ba) {
    builder.setInsertionPointToStart(b);
    auto Val = emitBlock(ba);

    auto *End = builder.getInsertionBlock();
    bool Terminated =
        !End->empty() &&
        End->back().hasTrait<mlir::OpTrait::IsTerminator>();
    if (!Terminated) {
      if (hasResult)
        mlir::cf::BranchOp::create(builder, location, mergeBlock,
                                   mlir::ValueRange{Val});
      else
        mlir::cf::BranchOp::create(builder, location, mergeBlock);
    }
    return Terminated;
  };
  auto thenTerminated = conf_helper(thenBlock, ast->thenBlockAST.get());
  auto elseTerminated = conf_helper(elseBlock, ast->elseBlockAST.get());
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

mlir::Value MLIRGenImpl::emitWhileExpr(AST::WhileExprAST *ast) {
  auto location = loc(ast);
  auto *parentRegion = builder.getInsertionBlock()->getParent();

  // Branch to header block
  auto *headerBlock = new mlir::Block();
  mlir::cf::BranchOp::create(builder, location, headerBlock);

  // --- Header block: evaluate condition ---
  parentRegion->push_back(headerBlock);
  builder.setInsertionPointToStart(headerBlock);
  auto cond = emitExpr(ast->condition.get());
  if (!cond)
    return nullptr;
  auto *bodyBlock = new mlir::Block();
  auto *exitBlock = new mlir::Block();
  mlir::cf::CondBranchOp::create(builder, location, cond, bodyBlock,
                                 /*trueArgs=*/{}, exitBlock,
                                 /*falseArgs=*/{});

  // --- Body block ---
  parentRegion->push_back(bodyBlock);
  builder.setInsertionPointToStart(bodyBlock);
  emitBlock(ast->body.get());

  // Back-edge to header (only if body didn't already terminate)
  auto *bodyEnd = builder.getInsertionBlock();
  bool bodyTerminated = !bodyEnd->empty() &&
                        bodyEnd->back().hasTrait<mlir::OpTrait::IsTerminator>();
  if (!bodyTerminated)
    mlir::cf::BranchOp::create(builder, location, headerBlock);

  // --- Exit block ---
  parentRegion->push_back(exitBlock);
  builder.setInsertionPointToStart(exitBlock);

  // While loops always evaluate to unit
  return nullptr;
}

mlir::Value MLIRGenImpl::emitStringExpr(AST::StringExprAST *ast) {
  auto name = fmt::format("{}{}", kStringPrefix, strCounter++);
  return getOrCreateGlobalString(name, ast->string_content, loc(ast));
}

// ===--- Array emission ---===

mlir::Value MLIRGenImpl::emitArrayLiteralExpr(AST::ArrayLiteralExprAST *ast) {
  auto location = loc(ast);
  auto arrType = std::get<ArrayType>(ast->get_type().type_data);
  auto llvmArrayType =
      mlir::cast<mlir::LLVM::LLVMArrayType>(convertType(ast->get_type()));

  // Stack-allocate the array via llvm.alloca
  auto alloca = emitAllocaOne(llvmArrayType, location);

  // Store each element via GEP + store
  for (size_t i = 0; i < ast->elements.size(); ++i) {
    auto elemVal = emitRValue(ast->elements[i].get());
    if (!elemVal)
      return nullptr;
    auto idx = mlir::arith::ConstantIntOp::create(
        builder, location, builder.getI32Type(), static_cast<int64_t>(i));
    emitPtrArrayStore(alloca, idx, elemVal, arrType, location);
  }

  return alloca;
}

mlir::Value MLIRGenImpl::emitIndexExpr(AST::IndexExprAST *ast) {
  auto location = loc(ast);
  auto arr = emitExpr(ast->array_expr.get());
  auto idx = emitExpr(ast->index_expr.get());
  if (!arr || !idx)
    return nullptr;

  auto baseType = ast->array_expr->get_type();

  // Pointer indexing: ptr<T>[i] -> GEP + load
  if (baseType.type_kind == TypeKind::Pointer) {
    auto &ptrData = std::get<PointerType>(baseType.type_data);
    auto pointee = ptrData.get_pointee();
    auto elemMlirType = convertType(pointee);
    auto gep = emitPtrElementGEP(arr, idx, pointee, location);
    return mlir::LLVM::LoadOp::create(builder, location, elemMlirType, gep);
  }

  // Array indexing (existing path)
  auto arrType = std::get<ArrayType>(baseType.type_data);

  // Cast i32 index to index type for bounds check
  auto indexVal = mlir::arith::IndexCastOp::create(builder, location,
                                                   builder.getIndexType(), idx);

  // Bounds check
  emitBoundsCheck(indexVal, arrType.get_size(), location);

  // If arr is an aggregate value (e.g. from struct field access) rather than
  // a pointer, spill it to a temp alloca so GEP can operate on it.
  arr = ensurePointer(arr, location);

  // All arrays are !llvm.ptr — use LLVM GEP + load
  return emitPtrArrayLoad(arr, idx, arrType, location);
}

mlir::Value MLIRGenImpl::emitPtrArrayGEP(mlir::Value ptr, mlir::Value idx,
                                         const ArrayType &arrType,
                                         mlir::Location location) {
  auto elemMlirType = convertType(arrType.get_element());
  auto arrLLVMType =
      mlir::LLVM::LLVMArrayType::get(elemMlirType, arrType.get_size());
  return mlir::LLVM::GEPOp::create(builder, location, llvmPtrTy(), arrLLVMType,
                                   ptr,
                                   llvm::ArrayRef<mlir::LLVM::GEPArg>{0, idx},
                                   mlir::LLVM::GEPNoWrapFlags::inbounds);
}

mlir::Value MLIRGenImpl::emitPtrArrayLoad(mlir::Value ptr, mlir::Value idx,
                                          const ArrayType &arrType,
                                          mlir::Location location) {
  auto gepOp = emitPtrArrayGEP(ptr, idx, arrType, location);
  return mlir::LLVM::LoadOp::create(builder, location,
                                    convertType(arrType.get_element()), gepOp);
}

void MLIRGenImpl::emitPtrArrayStore(mlir::Value ptr, mlir::Value idx,
                                    mlir::Value val, const ArrayType &arrType,
                                    mlir::Location location) {
  auto gepOp = emitPtrArrayGEP(ptr, idx, arrType, location);
  mlir::LLVM::StoreOp::create(builder, location, val, gepOp);
}

mlir::Value MLIRGenImpl::emitPtrElementGEP(mlir::Value ptr, mlir::Value idx,
                                           const Type &pointeeType,
                                           mlir::Location location) {
  auto elemMlirType = convertType(pointeeType);
  // Pointer arithmetic GEP: ptr + idx * sizeof(elemType)
  // Single index {idx}, unlike array GEP which uses {0, idx}
  return mlir::LLVM::GEPOp::create(
      builder, location, llvmPtrTy(), elemMlirType, ptr,
      llvm::ArrayRef<mlir::LLVM::GEPArg>{idx},
      mlir::LLVM::GEPNoWrapFlags::inbounds);
}

mlir::Value MLIRGenImpl::emitLenExpr(AST::LenExprAST *ast) {
  auto arrSize =
      std::get<ArrayType>(ast->operand->get_type().type_data).get_size();
  return mlir::arith::ConstantIntOp::create(builder, loc(ast),
                                            builder.getI32Type(),
                                            static_cast<int64_t>(arrSize))
      .getResult();
}

mlir::Value MLIRGenImpl::emitDimExpr(AST::DimExprAST *ast) {
  auto location = loc(ast);
  auto tupleTy = convertType(ast->get_type());

  // Walk nested ArrayType to collect dimension sizes
  std::vector<int64_t> dims;
  Type current = ast->operand->get_type();
  while (current.type_kind == TypeKind::Array) {
    auto &arr = std::get<ArrayType>(current.type_data);
    dims.push_back(static_cast<int64_t>(arr.get_size()));
    current = arr.get_element();
  }

  // Build a tuple of i32 constants
  mlir::Value agg = mlir::LLVM::PoisonOp::create(builder, location, tupleTy);
  for (size_t i = 0; i < dims.size(); i++) {
    auto val = mlir::arith::ConstantIntOp::create(builder, location,
                                                   builder.getI32Type(),
                                                   dims[i]).getResult();
    agg = mlir::LLVM::InsertValueOp::create(
        builder, location, agg, val,
        llvm::ArrayRef<int64_t>{static_cast<int64_t>(i)});
  }
  return agg;
}

// ===--- Pointer emission ---===

mlir::Value MLIRGenImpl::emitDerefExpr(AST::DerefExprAST *ast) {
  auto location = loc(ast);
  auto ptr = emitExpr(ast->operand.get());
  if (!ptr)
    return nullptr;

  auto ptrType = std::get<PointerType>(ast->operand->get_type().type_data);
  auto pointeeType = ptrType.get_pointee();

  // Pointer-to-array: return the raw !llvm.ptr unchanged.
  // Downstream consumers (emitIndexExpr, assign-to-index) detect the
  // !llvm.ptr MLIR type and use LLVM GEP + load/store, so this works
  // regardless of how deeply nested the deref expression is.
  if (pointeeType.type_kind == TypeKind::Array)
    return ptr;

  auto mlirPointeeType = convertType(pointeeType);
  auto loadedVal =
      mlir::LLVM::LoadOp::create(builder, location, mlirPointeeType, ptr);

  return loadedVal;
}

mlir::Value MLIRGenImpl::emitAddrOfExpr(AST::AddrOfExprAST *ast) {
  return emitLValue(ast->operand.get());
}

mlir::Value MLIRGenImpl::emitAllocExpr(AST::AllocExprAST *ast) {
  auto location = loc(ast);
  auto countVal = emitExpr(ast->operand.get());
  if (!countVal)
    return nullptr;

  // Extract pointee type T from result type ptr<T>
  auto ptrType = std::get<PointerType>(ast->get_type().type_data);
  int64_t elemSize = getTypeSize(ptrType.get_pointee());

  auto ptrTy = llvmPtrTy();
  auto i64Ty = builder.getI64Type();

  auto elemSizeVal =
      mlir::arith::ConstantIntOp::create(builder, location, i64Ty, elemSize);

  // Extend count to i64 if needed
  mlir::Value countI64 = countVal;
  if (countVal.getType() != i64Ty)
    countI64 = mlir::arith::ExtSIOp::create(builder, location, i64Ty, countVal);

  // total_size = sizeof(T) * count
  auto totalSize =
      mlir::arith::MulIOp::create(builder, location, elemSizeVal, countI64);

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

  mlir::LLVM::CallOp::create(builder, location, mlir::TypeRange{}, kFreeFunc,
                             mlir::ValueRange{ptr});

  return nullptr;
}

// ===--- Struct emission ---===

mlir::Value MLIRGenImpl::emitStructLiteralExpr(AST::StructLiteralExprAST *ast) {
  auto st = std::get<StructType>(ast->get_type().type_data);
  auto structTy = structTypes.at(st.get_name().mangled());
  auto location = loc(ast);

  // Start with poison (all fields will be filled by InsertValueOp)
  mlir::Value agg = mlir::LLVM::PoisonOp::create(builder, location, structTy);

  // Insert each field value at its index
  for (size_t i = 0; i < ast->field_values.size(); i++) {
    auto fieldIdx = st.get_field_index(ast->field_names[i]);
    auto val = emitRValue(ast->field_values[i].get());
    agg = mlir::LLVM::InsertValueOp::create(builder, location, agg, val,
                                            fieldIdx.value());
  }
  return agg;
}

mlir::Value MLIRGenImpl::emitFieldAccessExpr(AST::FieldAccessExprAST *ast) {
  auto objVal = emitExpr(ast->object_expr.get());
  auto st = std::get<StructType>(ast->object_expr->get_type().type_data);
  auto fieldIdx = st.get_field_index(ast->field_name);
  auto fieldType = convertType(st.get_field_type(fieldIdx.value()));
  return mlir::LLVM::ExtractValueOp::create(builder, loc(ast), fieldType,
                                            objVal, fieldIdx.value());
}

mlir::Value MLIRGenImpl::emitEnumConstructor(AST::CallExprAST *ast) {
  auto location = loc(ast);
  auto et = std::get<EnumType>(ast->get_type().type_data);
  auto callEnum = props_.call(ast->id());
  size_t variant_idx = callEnum->enum_variant_index;

  // Integer-backed enum: emit bare integer constant (no struct)
  if (et.is_integer_backed()) {
    auto &vi = et.get_variant(variant_idx);
    return mlir::arith::ConstantIntOp::create(builder, location,
                                              getEnumBackingMLIRType(et),
                                              vi.discriminant_value.value())
        .getResult();
  }

  auto enumTy = enumTypes.at(et.get_name().mangled());

  // Alloca the enum struct
  auto alloca = emitAllocaOne(enumTy, location);

  // Store the discriminant tag
  auto tagVal = mlir::arith::ConstantIntOp::create(
      builder, location, builder.getI32Type(),
      static_cast<int64_t>(variant_idx));
  auto tagPtr =
      mlir::LLVM::GEPOp::create(builder, location, llvmPtrTy(), enumTy, alloca,
                                llvm::ArrayRef<mlir::LLVM::GEPArg>{0, 0},
                                mlir::LLVM::GEPNoWrapFlags::inbounds);
  mlir::LLVM::StoreOp::create(builder, location, tagVal, tagPtr);

  // Store payload fields into the byte buffer
  auto &vi = et.get_variant(variant_idx);
  if (!vi.payload_types.empty()) {
    auto payloadPtr = mlir::LLVM::GEPOp::create(
        builder, location, llvmPtrTy(), enumTy, alloca,
        llvm::ArrayRef<mlir::LLVM::GEPArg>{0, 1},
        mlir::LLVM::GEPNoWrapFlags::inbounds);
    int64_t byte_offset = 0;
    for (size_t i = 0; i < ast->arguments.size(); i++) {
      auto argVal = emitRValue(ast->arguments[i].get());
      int64_t field_offset =
          advancePayloadOffset(byte_offset, vi.payload_types[i]);
      mlir::Value dest;
      if (field_offset == 0) {
        dest = payloadPtr;
      } else {
        dest = mlir::LLVM::GEPOp::create(
            builder, location, llvmPtrTy(), builder.getI8Type(), payloadPtr,
            llvm::ArrayRef<mlir::LLVM::GEPArg>{
                static_cast<int32_t>(field_offset)},
            mlir::LLVM::GEPNoWrapFlags::inbounds);
      }
      mlir::LLVM::StoreOp::create(builder, location, argVal, dest);
    }
  }

  // Load and return the complete enum value
  return mlir::LLVM::LoadOp::create(builder, location, enumTy, alloca);
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
                                      /*resultTypes=*/mlir::TypeRange{}, oob,
                                      /*withElseRegion=*/false);

  // The IfOp constructor auto-inserts a yield terminator in the then block.
  // Set insertion point before it so our ops go before the yield.
  auto &thenBlock = ifOp.getThenRegion().front();
  builder.setInsertionPoint(&thenBlock, thenBlock.begin());

  // exit(1) — uses func.call since exit is declared as func.func
  auto one = mlir::arith::ConstantIntOp::create(builder, location, i32Ty, 1);
  mlir::func::CallOp::create(builder, location, kExitFunc, mlir::TypeRange{},
                             mlir::ValueRange{one});

  builder.setInsertionPointAfter(ifOp);
}

mlir::Value MLIRGenImpl::emitArrayComparison(mlir::Value lhs, mlir::Value rhs,
                                             const Type &arrType, TokenType tok,
                                             mlir::Location location) {
  auto &arrData = std::get<ArrayType>(arrType.type_data);
  int64_t size = static_cast<int64_t>(arrData.get_size());
  auto i1Ty = builder.getI1Type();
  auto indexTy = builder.getIndexType();

  auto *parentRegion = builder.getInsertionBlock()->getParent();

  // header takes a loop counter (index type)
  auto *headerBlock = new mlir::Block();
  headerBlock->addArgument(indexTy, location);

  // exit takes the result (i1)
  auto *exitBlock = new mlir::Block();
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
  auto *bodyBlock = new mlir::Block();
  mlir::cf::CondBranchOp::create(builder, location, cond, bodyBlock, {},
                                 exitBlock, mlir::ValueRange{trueVal});

  // --- Body block: compare elements[i] ---
  parentRegion->push_back(bodyBlock);
  builder.setInsertionPointToStart(bodyBlock);
  // Cast index counter to i32 for emitPtrArrayLoad
  auto counterI32 = mlir::arith::IndexCastOp::create(
      builder, location, builder.getI32Type(), counter);
  auto lhsElem = emitPtrArrayLoad(lhs, counterI32, arrData, location);
  auto rhsElem = emitPtrArrayLoad(rhs, counterI32, arrData, location);
  // Compare elements
  mlir::Value elemEq;
  if (isIntegerType(arrData.get_element()) || isBoolType(arrData.get_element()))
    elemEq = mlir::arith::CmpIOp::create(
        builder, location, mlir::arith::CmpIPredicate::eq, lhsElem, rhsElem);
  else if (isFloatType(arrData.get_element()))
    elemEq = mlir::arith::CmpFOp::create(
        builder, location, mlir::arith::CmpFPredicate::OEQ, lhsElem, rhsElem);
  else if (arrData.get_element().type_kind == TypeKind::Pointer) {
    // Pointer equality: compare as integers
    auto i64Ty = builder.getI64Type();
    auto lhsInt =
        mlir::LLVM::PtrToIntOp::create(builder, location, i64Ty, lhsElem);
    auto rhsInt =
        mlir::LLVM::PtrToIntOp::create(builder, location, i64Ty, rhsElem);
    elemEq = mlir::arith::CmpIOp::create(
        builder, location, mlir::arith::CmpIPredicate::eq, lhsInt, rhsInt);
  } else
    imm_error("unsupported array element type for comparison");

  // If equal → increment and loop back to header; else → mismatch
  auto one = mlir::arith::ConstantIndexOp::create(builder, location, 1);
  auto next = mlir::arith::AddIOp::create(builder, location, counter, one);
  auto *mismatchBlock = new mlir::Block();
  mlir::cf::CondBranchOp::create(builder, location, elemEq, headerBlock,
                                 mlir::ValueRange{next.getResult()},
                                 mismatchBlock, {});

  // --- Mismatch block: exit with false ---
  parentRegion->push_back(mismatchBlock);
  builder.setInsertionPointToStart(mismatchBlock);
  auto falseVal =
      mlir::arith::ConstantIntOp::create(builder, location, i1Ty, 0);
  mlir::cf::BranchOp::create(builder, location, exitBlock,
                             mlir::ValueRange{falseVal});

  // --- Exit block: result via block argument ---
  parentRegion->push_back(exitBlock);
  builder.setInsertionPointToStart(exitBlock);
  auto result = exitBlock->getArgument(0);

  // For !=, negate the result
  if (tok == TokNOTEqual) {
    auto trueConst =
        mlir::arith::ConstantIntOp::create(builder, location, i1Ty, 1);
    return mlir::arith::XOrIOp::create(builder, location, result, trueConst)
        .getResult();
  }
  return result;
}

mlir::Value MLIRGenImpl::emitTupleLiteralExpr(AST::TupleLiteralExprAST *ast) {
  auto location = loc(ast);
  auto tupleTy = convertType(ast->get_type());

  // Start with poison (all elements will be filled by InsertValueOp)
  mlir::Value agg = mlir::LLVM::PoisonOp::create(builder, location, tupleTy);

  // Insert each element at its index
  for (size_t i = 0; i < ast->elements.size(); i++) {
    auto val = emitRValue(ast->elements[i].get());
    agg = mlir::LLVM::InsertValueOp::create(
        builder, location, agg, val,
        llvm::ArrayRef<int64_t>{static_cast<int64_t>(i)});
  }
  return agg;
}

mlir::Value MLIRGenImpl::emitCaseExpr(AST::CaseExprAST *ast) {
  auto location = loc(ast);

  // 1. Emit scrutinee
  auto scrutineeVal = emitExpr(ast->scrutinee.get());

  // Dispatch: literal patterns use a separate path (no enum type)
  if (ast->scrutinee->get_type().type_kind != TypeKind::Enum)
    return emitLiteralCaseExpr(ast, scrutineeVal);

  auto et = std::get<EnumType>(ast->scrutinee->get_type().type_data);

  // Integer-backed enum: scrutinee IS the tag, no struct to decompose
  if (et.is_integer_backed())
    return emitIntegerBackedCaseExpr(ast, scrutineeVal, et);

  auto enumTy = enumTypes.at(et.get_name().mangled());

  // Alloca the scrutinee so we can GEP into it
  auto scrutineeAlloca = emitAllocaOne(enumTy, location);
  mlir::LLVM::StoreOp::create(builder, location, scrutineeVal, scrutineeAlloca);

  // 2. Extract the tag
  auto tagPtr = mlir::LLVM::GEPOp::create(
      builder, location, llvmPtrTy(), enumTy, scrutineeAlloca,
      llvm::ArrayRef<mlir::LLVM::GEPArg>{0, 0},
      mlir::LLVM::GEPNoWrapFlags::inbounds);
  auto tag = mlir::LLVM::LoadOp::create(builder, location, builder.getI32Type(),
                                        tagPtr);

  bool hasResult = ast->get_type().type_kind != TypeKind::Unit &&
                   ast->get_type().type_kind != TypeKind::Never;

  auto *parentRegion = builder.getInsertionBlock()->getParent();

  // 3. Create blocks for each arm + merge
  std::vector<mlir::Block *> armBlocks;
  mlir::Block *defaultBlock = nullptr;

  for (size_t i = 0; i < ast->arms.size(); i++) {
    armBlocks.push_back(addBlock(parentRegion));
    if (ast->arms[i].pattern.is_wildcard)
      defaultBlock = armBlocks.back();
  }

  auto *mergeBlock = new mlir::Block();
  if (hasResult) {
    // Array expressions produce !llvm.ptr (alloca pointers), not LLVMArrayType
    auto mergeType = ast->get_type().type_kind == TypeKind::Array
                         ? mlir::Type(llvmPtrTy())
                         : convertType(ast->get_type());
    mergeBlock->addArgument(mergeType, location);
  }

  // If no wildcard, create an unreachable default
  if (!defaultBlock) {
    defaultBlock = addBlock(parentRegion);
    builder.setInsertionPointToStart(defaultBlock);
    mlir::LLVM::UnreachableOp::create(builder, location);
  }

  // 4. Emit switch via cascading cmpi + cond_br
  // (LLVM::SwitchOp is not always available in LLVM dialect, so use cf
  // branches)
  builder.setInsertionPointAfter(tag.getOperation());

  // Build a chain: for each non-wildcard arm, compare tag and branch
  // Final fallthrough goes to default block
  std::vector<size_t> nonWildcardIndices;
  for (size_t i = 0; i < ast->arms.size(); i++) {
    if (!ast->arms[i].pattern.is_wildcard)
      nonWildcardIndices.push_back(i);
  }

  for (size_t ci = 0; ci < nonWildcardIndices.size(); ci++) {
    size_t i = nonWildcardIndices[ci];
    auto &arm = ast->arms[i];
    auto tagConst = mlir::arith::ConstantIntOp::create(
        builder, location, builder.getI32Type(),
        static_cast<int64_t>(arm.pattern.variant_index));
    auto cmp = mlir::arith::CmpIOp::create(
        builder, location, mlir::arith::CmpIPredicate::eq, tag, tagConst);

    // If this is the last non-wildcard, the false branch goes to default
    mlir::Block *falseTarget;
    if (ci + 1 < nonWildcardIndices.size()) {
      falseTarget = addBlock(parentRegion);
    } else {
      falseTarget = defaultBlock;
    }

    mlir::cf::CondBranchOp::create(builder, location, cmp, armBlocks[i], {},
                                   falseTarget, {});

    if (ci + 1 < nonWildcardIndices.size()) {
      builder.setInsertionPointToStart(falseTarget);
    }
  }

  // If there are no non-wildcard arms, branch directly to default
  if (nonWildcardIndices.empty()) {
    mlir::cf::BranchOp::create(builder, location, defaultBlock);
  }

  // 5. Emit each arm body
  bool allTerminated = true;

  for (size_t i = 0; i < ast->arms.size(); i++) {
    auto &arm = ast->arms[i];
    builder.setInsertionPointToStart(armBlocks[i]);

    // Extract payload bindings
    if (!arm.pattern.is_wildcard && !arm.pattern.bindings.empty()) {
      auto &vi = et.get_variant(arm.pattern.variant_index);
      auto payloadPtr = mlir::LLVM::GEPOp::create(
          builder, location, llvmPtrTy(), enumTy, scrutineeAlloca,
          llvm::ArrayRef<mlir::LLVM::GEPArg>{0, 1},
          mlir::LLVM::GEPNoWrapFlags::inbounds);

      int64_t byte_offset = 0;
      for (size_t j = 0; j < arm.pattern.bindings.size(); j++) {
        auto fieldMlirTy = convertType(vi.payload_types[j]);
        int64_t field_offset =
            advancePayloadOffset(byte_offset, vi.payload_types[j]);
        mlir::Value fieldPtr;
        if (field_offset == 0) {
          fieldPtr = payloadPtr;
        } else {
          fieldPtr = mlir::LLVM::GEPOp::create(
              builder, location, llvmPtrTy(), builder.getI8Type(), payloadPtr,
              llvm::ArrayRef<mlir::LLVM::GEPArg>{
                  static_cast<int32_t>(field_offset)},
              mlir::LLVM::GEPNoWrapFlags::inbounds);
        }
        auto fieldVal = mlir::LLVM::LoadOp::create(builder, location,
                                                   fieldMlirTy, fieldPtr);

        // Wrap in alloca+store to match uniform variable model
        // (emitVariableExpr expects all !llvm.ptr values to be allocas)
        auto alloca = emitAllocaOne(fieldMlirTy, location);
        mlir::LLVM::StoreOp::create(builder, location, fieldVal, alloca);
        symbolTable.registerNameT(arm.pattern.bindings[j], alloca);
      }
    }

    // Emit body
    auto armVal = emitBlock(arm.body.get());

    auto *armEnd = builder.getInsertionBlock();
    bool armTerminated = !armEnd->empty() &&
                         armEnd->back().hasTrait<mlir::OpTrait::IsTerminator>();
    if (!armTerminated) {
      if (hasResult)
        mlir::cf::BranchOp::create(builder, location, mergeBlock,
                                   mlir::ValueRange{armVal});
      else
        mlir::cf::BranchOp::create(builder, location, mergeBlock);
      allTerminated = false;
    } else {
      allTerminated = allTerminated && true;
    }
  }

  // 6. Merge block
  if (!allTerminated) {
    parentRegion->push_back(mergeBlock);
    builder.setInsertionPointToStart(mergeBlock);
    return hasResult ? mergeBlock->getArgument(0) : nullptr;
  } else {
    delete mergeBlock;
    return nullptr;
  }
}

mlir::Value MLIRGenImpl::emitIntegerBackedCaseExpr(AST::CaseExprAST *ast,
                                                   mlir::Value tag,
                                                   const EnumType &et) {
  // Each arm's comparison constant is the variant's discriminant value.
  return emitScalarCaseExpr(ast, tag,
      [&](const AST::CaseArm &arm, mlir::Location loc) {
        auto &vi = et.get_variant(arm.pattern.variant_index);
        return mlir::arith::ConstantIntOp::create(
            builder, loc, getEnumBackingMLIRType(et),
            vi.discriminant_value.value());
      });
}

mlir::Value MLIRGenImpl::emitLiteralCaseExpr(AST::CaseExprAST *ast,
                                              mlir::Value scrutineeVal) {
  auto scrutineeType = ast->scrutinee->get_type();

  // Each arm's comparison constant is the literal value parsed from source.
  return emitScalarCaseExpr(ast, scrutineeVal,
      [&](const AST::CaseArm &arm, mlir::Location loc) -> mlir::Value {
        using LK = AST::CasePattern::LiteralKind;
        switch (arm.pattern.literal_kind) {
        case LK::Bool: {
          int64_t val = (arm.pattern.literal_value == "true") ? 1 : 0;
          return mlir::arith::ConstantIntOp::create(builder, loc, val, 1);
        }
        case LK::Char: {
          auto charVal = static_cast<uint8_t>(arm.pattern.literal_value[0]);
          return mlir::arith::ConstantIntOp::create(builder, loc, charVal, 8);
        }
        case LK::Integer: {
          int64_t val = std::stoll(arm.pattern.literal_value);
          return mlir::arith::ConstantIntOp::create(
              builder, loc, convertType(scrutineeType), val);
        }
        default:
          imm_error("unexpected literal kind in case pattern",
                    arm.pattern.location);
        }
      });
}

mlir::Value MLIRGenImpl::emitScalarCaseExpr(AST::CaseExprAST *ast,
                                             mlir::Value scrutineeVal,
                                             ArmToComparisonConst armToConst) {
  auto location = loc(ast);
  bool hasResult = ast->get_type().type_kind != TypeKind::Unit &&
                   ast->get_type().type_kind != TypeKind::Never;

  auto *parentRegion = builder.getInsertionBlock()->getParent();

  // Create blocks for each arm + merge
  std::vector<mlir::Block *> armBlocks;
  mlir::Block *defaultBlock = nullptr;

  for (size_t i = 0; i < ast->arms.size(); i++) {
    armBlocks.push_back(addBlock(parentRegion));
    if (ast->arms[i].pattern.is_wildcard)
      defaultBlock = armBlocks.back();
  }

  auto *mergeBlock = new mlir::Block();
  if (hasResult) {
    auto mergeType = ast->get_type().type_kind == TypeKind::Array
                         ? mlir::Type(llvmPtrTy())
                         : convertType(ast->get_type());
    mergeBlock->addArgument(mergeType, location);
  }

  if (!defaultBlock) {
    defaultBlock = addBlock(parentRegion);
    builder.setInsertionPointToStart(defaultBlock);
    mlir::LLVM::UnreachableOp::create(builder, location);
  }

  // Restore insertion point after scrutinee
  if (auto *defOp = scrutineeVal.getDefiningOp())
    builder.setInsertionPointAfter(defOp);
  else
    builder.setInsertionPointToEnd(scrutineeVal.getParentBlock());

  // Emit cascading cmpi + cond_br
  std::vector<size_t> nonWildcardIndices;
  for (size_t i = 0; i < ast->arms.size(); i++) {
    if (!ast->arms[i].pattern.is_wildcard)
      nonWildcardIndices.push_back(i);
  }

  for (size_t ci = 0; ci < nonWildcardIndices.size(); ci++) {
    size_t i = nonWildcardIndices[ci];
    auto armConst = armToConst(ast->arms[i], location);
    auto cmp = mlir::arith::CmpIOp::create(
        builder, location, mlir::arith::CmpIPredicate::eq, scrutineeVal,
        armConst);

    mlir::Block *falseTarget;
    if (ci + 1 < nonWildcardIndices.size()) {
      falseTarget = addBlock(parentRegion);
    } else {
      falseTarget = defaultBlock;
    }

    mlir::cf::CondBranchOp::create(builder, location, cmp, armBlocks[i], {},
                                   falseTarget, {});

    if (ci + 1 < nonWildcardIndices.size()) {
      builder.setInsertionPointToStart(falseTarget);
    }
  }

  if (nonWildcardIndices.empty()) {
    mlir::cf::BranchOp::create(builder, location, defaultBlock);
  }

  // Emit each arm body
  bool allTerminated = true;

  for (size_t i = 0; i < ast->arms.size(); i++) {
    auto &arm = ast->arms[i];
    builder.setInsertionPointToStart(armBlocks[i]);

    auto armVal = emitBlock(arm.body.get());

    auto *armEnd = builder.getInsertionBlock();
    bool armTerminated = !armEnd->empty() &&
                         armEnd->back().hasTrait<mlir::OpTrait::IsTerminator>();
    if (!armTerminated) {
      if (hasResult)
        mlir::cf::BranchOp::create(builder, location, mergeBlock,
                                   mlir::ValueRange{armVal});
      else
        mlir::cf::BranchOp::create(builder, location, mergeBlock);
      allTerminated = false;
    }
  }

  // Merge block
  if (!allTerminated) {
    parentRegion->push_back(mergeBlock);
    builder.setInsertionPointToStart(mergeBlock);
    return hasResult ? mergeBlock->getArgument(0) : nullptr;
  } else {
    delete mergeBlock;
    return nullptr;
  }
}

} // namespace sammine_lang
