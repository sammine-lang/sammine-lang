#include "codegen/MIRLowering.h"
#include "codegen/MLIRGenBinaryOps.h"
#include "util/Utilities.h"

#include "mlir/Dialect/Arith/IR/Arith.h"

//! \file MIRLowering.cpp
//! \brief The constructs MIR handles so far: functions over integers, integer
//! literals, parameters, `let`, arithmetic and return.

namespace sammine_lang {

using namespace AST;

std::optional<mlir::Type> MIRLowering::mirType(const Type &type) const {
  switch (type.type_kind) {
  case TypeKind::I32_t:
  case TypeKind::U32_t:
  case TypeKind::Integer: // an untyped literal defaults to i32, as in codegen
    return builder_.getI32Type();
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return builder_.getI64Type();
  default:
    return std::nullopt;
  }
}

// The signature `func` already has came from the AST codegen; MIR only takes
// functions whose every parameter and result it has a type for, so the two
// agree.
bool MIRLowering::supportsSignature(FuncDefAST *ast) const {
  auto *proto = ast->Prototype.get();
  if (proto->is_var_arg ||
      proto->get_type().type_kind != TypeKind::Function)
    return false;
  for (auto &param : proto->parameterVectors)
    if (!mirType(param->get_type()))
      return false;
  auto fn = std::get<FunctionType>(proto->get_type().type_data);
  return mirType(fn.get_return_type()).has_value();
}

bool MIRLowering::lowerBody(FuncDefAST *ast, mlir::func::FuncOp func) {
  if (!supportsSignature(ast))
    return false;

  mlir::OpBuilder::InsertionGuard guard(builder_);
  auto &entry = *func.addEntryBlock();
  builder_.setInsertionPointToStart(&entry);

  names_.clear();
  for (auto [param, arg] :
       llvm::zip(ast->Prototype->parameterVectors, entry.getArguments()))
    names_[param->name] = arg;

  if (mlir::failed(lowerBlock(ast->Block.get()))) {
    // Leave no trace: the AST codegen gives `func` its body instead.
    func.getBody().getBlocks().clear();
    return false;
  }
  func->setAttr(kMirAttr, builder_.getUnitAttr());
  return true;
}

// For now a body is `let`s, then its return:
// `{ let b = a * 2; b + 1 }` or `{ let b = a * 2; return b + 1; }`.
mlir::LogicalResult MIRLowering::lowerBlock(BlockAST *block) {
  if (block->Statements.empty())
    return mlir::failure();
  for (auto &stmt : llvm::drop_end(block->Statements)) {
    auto *def = llvm::dyn_cast<VarDefAST>(stmt.get());
    if (!def || mlir::failed(lowerVarDef(def)))
      return mlir::failure();
  }
  auto *ret = llvm::dyn_cast<ReturnStmtAST>(block->Statements.back().get());
  if (!ret)
    return mlir::failure();
  return lowerReturn(ret);
}

// A `let` of a value MIR has a type for names that value: the right side is
// lowered first, so `let x = x + 1` reads the x it shadows.
mlir::LogicalResult MIRLowering::lowerVarDef(VarDefAST *def) {
  if (def->is_tuple_destructure || !mirType(def->get_type()))
    return mlir::failure();
  auto value = lowerExpr(def->Expression.get());
  if (mlir::failed(value))
    return mlir::failure();
  names_[def->TypedVar->name] = *value;
  return mlir::success();
}

mlir::LogicalResult MIRLowering::lowerReturn(ReturnStmtAST *ret) {
  if (!ret->return_expr)
    return mlir::failure();
  auto value = lowerExpr(ret->return_expr.get());
  if (mlir::failed(value))
    return mlir::failure();
  mlir::func::ReturnOp::create(builder_, loc(ret), mlir::ValueRange{*value});
  return mlir::success();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerExpr(ExprAST *expr) {
  if (auto *num = llvm::dyn_cast<NumberExprAST>(expr))
    return lowerNumber(num);
  if (auto *var = llvm::dyn_cast<VariableExprAST>(expr))
    return lowerVariable(var);
  if (auto *bin = llvm::dyn_cast<BinaryExprAST>(expr))
    return lowerBinary(bin);
  return mlir::failure();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerNumber(NumberExprAST *num) {
  auto type = mirType(num->get_type());
  if (!type)
    return mlir::failure();
  return mlir::arith::ConstantIntOp::create(
             builder_, loc(num), *type,
             sammine_util::literal_int_bits(num->number))
      .getResult();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerVariable(VariableExprAST *var) {
  auto it = names_.find(var->variableName.mangled());
  if (it == names_.end())
    return mlir::failure();
  return it->second;
}

// Integer arithmetic through a built-in instance; an instance the program
// declares is a call, which MIR doesn't lower yet.
mlir::FailureOr<mlir::Value> MIRLowering::lowerBinary(BinaryExprAST *bin) {
  auto *bp = props_.binary(bin->id());
  if (bp && bp->resolved_op_method)
    return mlir::failure();
  auto op = bin->Op->tok_type;
  bool arithmetic = op == TokADD || op == TokSUB || op == TokMUL ||
                    op == TokDIV || op == TokMOD;
  auto type = bin->get_type();
  if (!arithmetic || !mirType(type))
    return mlir::failure();
  auto lhs = lowerExpr(bin->LHS.get());
  if (mlir::failed(lhs))
    return mlir::failure();
  auto rhs = lowerExpr(bin->RHS.get());
  if (mlir::failed(rhs))
    return mlir::failure();
  return emitIntArithOp(builder_, loc(bin), *lhs, *rhs, op,
                        type.is_unsigned());
}

} // namespace sammine_lang
