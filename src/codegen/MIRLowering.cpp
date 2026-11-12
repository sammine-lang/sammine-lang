#include "codegen/MIRLowering.h"
#include "codegen/MLIRGenBinaryOps.h"
#include "dialect/SammineDialect.h"
#include "util/Utilities.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/OwningOpRef.h"

//! \file MIRLowering.cpp
//! \brief The constructs MIR handles so far: functions over scalars (integers,
//! floats, bool, char) and pointers to them, their literals, parameters,
//! `let`, arithmetic and return.

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
  case TypeKind::F32_t:
    return builder_.getF32Type();
  case TypeKind::F64_t:
  case TypeKind::Flt: // an untyped float literal defaults to f64
    return builder_.getF64Type();
  case TypeKind::Bool:
    return builder_.getI1Type();
  case TypeKind::Char:
    return builder_.getI8Type();
  case TypeKind::Pointer: {
    // 'ptr<T> owns what it points at; ptr<T> is a view of it.
    auto pointee = mirType(std::get<PointerType>(type.type_data).get_pointee());
    if (!pointee)
      return std::nullopt;
    return smn::PtrType::get(builder_.getContext(), *pointee,
                             type.linearity == Linearity::Linear
                                 ? smn::Ownership::Owned
                                 : smn::Ownership::View);
  }
  default:
    return std::nullopt;
  }
}

std::optional<mlir::FunctionType>
MIRLowering::mirSignature(FuncDefAST *ast) const {
  auto *proto = ast->Prototype.get();
  if (proto->is_var_arg || proto->get_type().type_kind != TypeKind::Function)
    return std::nullopt;
  llvm::SmallVector<mlir::Type, 4> inputs;
  for (auto &param : proto->parameterVectors) {
    auto type = mirType(param->get_type());
    if (!type)
      return std::nullopt;
    inputs.push_back(*type);
  }
  auto fn = std::get<FunctionType>(proto->get_type().type_data);
  auto result = mirType(fn.get_return_type());
  if (!result)
    return std::nullopt;
  return builder_.getFunctionType(inputs, {*result});
}

// The function is built on its own, outside the module, and only put in
// place of `decl` once it is whole: a body MIR can't lower is dropped with
// it, and `decl` was never touched.
bool MIRLowering::lowerFunction(FuncDefAST *ast, mlir::func::FuncOp decl) {
  auto signature = mirSignature(ast);
  if (!signature)
    return false;

  mlir::OwningOpRef<mlir::func::FuncOp> func =
      mlir::func::FuncOp::create(loc(ast), decl.getSymName(), *signature);
  func->setSymVisibilityAttr(decl.getSymVisibilityAttr());

  mlir::OpBuilder::InsertionGuard guard(builder_);
  auto &entry = *func->addEntryBlock();
  builder_.setInsertionPointToStart(&entry);

  names_.clear();
  for (auto [param, arg] :
       llvm::zip(ast->Prototype->parameterVectors, entry.getArguments()))
    names_[param->name] = arg;

  if (mlir::failed(lowerBlock(ast->Block.get())))
    return false;

  (*func)->setAttr(kMirAttr, builder_.getUnitAttr());
  decl->getBlock()->getOperations().insert(mlir::Block::iterator(decl),
                                           func.release());
  decl.erase();
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
  if (auto *b = llvm::dyn_cast<BoolExprAST>(expr))
    return lowerBool(b);
  if (auto *c = llvm::dyn_cast<CharExprAST>(expr))
    return lowerChar(c);
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
  if (auto floatType = mlir::dyn_cast<mlir::FloatType>(*type))
    return emitFloatConstant(builder_, loc(num), std::stod(num->number),
                             floatType);
  return mlir::arith::ConstantIntOp::create(
             builder_, loc(num), *type,
             sammine_util::literal_int_bits(num->number))
      .getResult();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerBool(BoolExprAST *b) {
  return mlir::arith::ConstantIntOp::create(builder_, loc(b),
                                            builder_.getI1Type(), b->b ? 1 : 0)
      .getResult();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerChar(CharExprAST *c) {
  return mlir::arith::ConstantIntOp::create(builder_, loc(c),
                                            builder_.getI8Type(),
                                            static_cast<uint8_t>(c->value))
      .getResult();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerVariable(VariableExprAST *var) {
  auto it = names_.find(var->variableName.mangled());
  if (it == names_.end())
    return mlir::failure();
  return it->second;
}

// Integer or float arithmetic through a built-in instance; an instance the
// program declares is a call, which MIR doesn't lower yet.
mlir::FailureOr<mlir::Value> MIRLowering::lowerBinary(BinaryExprAST *bin) {
  auto *bp = props_.binary(bin->id());
  if (bp && bp->resolved_op_method)
    return mlir::failure();
  auto op = bin->Op->tok_type;
  bool arithmetic = op == TokADD || op == TokSUB || op == TokMUL ||
                    op == TokDIV || op == TokMOD;
  auto type = bin->get_type();
  if (!arithmetic || !(type.is_integer() || type.is_float()))
    return mlir::failure();
  auto lhs = lowerExpr(bin->LHS.get());
  if (mlir::failed(lhs))
    return mlir::failure();
  auto rhs = lowerExpr(bin->RHS.get());
  if (mlir::failed(rhs))
    return mlir::failure();
  if (type.is_float())
    return emitFloatArithOp(builder_, loc(bin), *lhs, *rhs, op);
  return emitIntArithOp(builder_, loc(bin), *lhs, *rhs, op,
                        type.is_unsigned());
}

} // namespace sammine_lang
