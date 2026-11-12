#include "codegen/MIRLowering.h"
#include "codegen/MLIRGenBinaryOps.h"
#include "dialect/SammineDialect.h"
#include "util/Utilities.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/OwningOpRef.h"

//! \file MIRLowering.cpp
//! \brief The constructs MIR handles so far: functions over scalars (integers,
//! floats, bool, char) and pointers to them, returning one or `()`; their
//! literals, parameters, `let`, arithmetic, calls to other sammine
//! functions, `alloc`, `free`, `*p` read and written, statements and return.

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
MIRLowering::mirFunctionType(const FunctionType &fn) const {
  if (fn.is_var_arg())
    return std::nullopt;
  llvm::SmallVector<mlir::Type, 4> inputs;
  for (auto &param : fn.get_params_types()) {
    auto type = mirType(param);
    if (!type)
      return std::nullopt;
    inputs.push_back(*type);
  }
  // A function returning () returns nothing.
  if (fn.get_return_type().type_kind == TypeKind::Unit)
    return builder_.getFunctionType(inputs, {});
  auto result = mirType(fn.get_return_type());
  if (!result)
    return std::nullopt;
  return builder_.getFunctionType(inputs, {*result});
}

std::optional<mlir::FunctionType>
MIRLowering::mirSignature(FuncDefAST *ast) const {
  auto type = ast->Prototype->get_type();
  if (type.type_kind != TypeKind::Function)
    return std::nullopt;
  return mirFunctionType(std::get<FunctionType>(type.type_data));
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

  module_ = decl->getParentOfType<mlir::ModuleOp>();
  names_.clear();
  for (auto [param, arg] :
       llvm::zip(ast->Prototype->parameterVectors, entry.getArguments()))
    names_[param->name] = arg;

  if (mlir::failed(lowerBlock(ast->Block.get())))
    return false;

  (*func)->setAttr(smn::kMirAttr, builder_.getUnitAttr());
  decl->getBlock()->getOperations().insert(mlir::Block::iterator(decl),
                                           func.release());
  decl.erase();
  return true;
}

// For now a body is `let`s and statements, then its return:
// `{ let b = a * 2; free(p); b + 1 }`. A statement's value, if any, is dropped.
mlir::LogicalResult MIRLowering::lowerBlock(BlockAST *block) {
  if (block->Statements.empty())
    return mlir::failure();
  for (auto &stmt : llvm::drop_end(block->Statements)) {
    if (auto *def = llvm::dyn_cast<VarDefAST>(stmt.get())) {
      if (mlir::failed(lowerVarDef(def)))
        return mlir::failure();
    } else if (mlir::failed(lowerExpr(stmt.get()))) {
      return mlir::failure();
    }
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
  // Returning () returns nothing.
  mlir::func::ReturnOp::create(builder_, loc(ret),
                               *value ? mlir::ValueRange{*value}
                                      : mlir::ValueRange{});
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
    return bin->Op->is_assign() ? lowerAssign(bin) : lowerBinary(bin);
  if (auto *call = llvm::dyn_cast<CallExprAST>(expr))
    return lowerCall(call);
  if (auto *alloc = llvm::dyn_cast<AllocExprAST>(expr))
    return lowerAlloc(alloc);
  if (auto *free = llvm::dyn_cast<FreeExprAST>(expr))
    return lowerFree(free);
  if (auto *deref = llvm::dyn_cast<DerefExprAST>(expr))
    return lowerDerefRead(deref);
  if (llvm::isa<UnitExprAST>(expr))
    return mlir::Value();
  return mlir::failure();
}

// `f(a, b)`, `f` a sammine function, a generic function's copy or an
// instance's method, whether its body goes through MIR or not.
mlir::FailureOr<mlir::Value> MIRLowering::lowerCall(CallExprAST *call) {
  // Not lowered yet: enum constructors, instances the compiler provides, and
  // partial application.
  auto *cp = props_.call(call->id());
  if (cp->is_enum_constructor || cp->builtin_instance || cp->is_partial)
    return mlir::failure();
  // Nor a call through a function value or to a C function, which are not
  // func.func symbols.
  auto name =
      (cp->resolved_name ? *cp->resolved_name : call->functionName).mangled();
  if (!module_.lookupSymbol<mlir::func::FuncOp>(name))
    return mlir::failure();
  auto type =
      mirFunctionType(std::get<FunctionType>(cp->callee_func_type->type_data));
  if (!type)
    return mlir::failure();

  llvm::SmallVector<mlir::Value, 4> args;
  for (auto [arg, param] : llvm::zip(call->arguments, type->getInputs())) {
    auto value = lowerExpr(arg.get());
    // An argument of another type is an owned pointer lent to a ptr<T>,
    // which isn't lowered yet.
    if (mlir::failed(value) || !*value || value->getType() != param)
      return mlir::failure();
    args.push_back(*value);
  }
  auto op = smn::CallOp::create(builder_, loc(call), type->getResults(), name,
                                args);
  return op.getNumResults() ? op.getResult(0) : mlir::Value();
}

// alloc<T>(n): an owned pointer to room for n values of T, none stored yet.
mlir::FailureOr<mlir::Value> MIRLowering::lowerAlloc(AllocExprAST *alloc) {
  auto type = mirType(alloc->get_type());
  auto count = lowerExpr(alloc->operand.get());
  if (!type || mlir::failed(count) || !*count)
    return mlir::failure();
  return smn::AllocOp::create(builder_, loc(alloc), *type, *count).getResult();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerFree(FreeExprAST *free) {
  auto ptr = lowerExpr(free->operand.get());
  if (mlir::failed(ptr) || !*ptr)
    return mlir::failure();
  smn::FreeOp::create(builder_, loc(free), *ptr);
  return mlir::Value();
}

// The place `*p` is, for `p` a pointer MIR has a type for.
mlir::FailureOr<mlir::Value> MIRLowering::lowerDerefPlace(DerefExprAST *deref) {
  if (!mirType(deref->operand->get_type()))
    return mlir::failure();
  auto ptr = lowerExpr(deref->operand.get());
  if (mlir::failed(ptr) || !*ptr)
    return mlir::failure();
  auto pointee = mlir::cast<smn::PtrType>(ptr->getType()).getPointee();
  return smn::DerefOp::create(builder_, loc(deref),
                              smn::PlaceType::get(builder_.getContext(), pointee),
                              *ptr)
      .getResult();
}

// Reading `*p` copies a value that owns nothing, and moves one that does out
// of the place.
mlir::FailureOr<mlir::Value> MIRLowering::lowerDerefRead(DerefExprAST *deref) {
  auto place = lowerDerefPlace(deref);
  if (mlir::failed(place))
    return mlir::failure();
  auto valueType = mlir::cast<smn::PlaceType>(place->getType()).getValueType();
  if (smn::ownsMemory(valueType))
    return smn::MoveOp::create(builder_, loc(deref), valueType, *place)
        .getResult();
  return smn::CopyOp::create(builder_, loc(deref), valueType, *place)
      .getResult();
}

// `*p = v`. Writing anywhere else, a field or an element, isn't lowered yet.
mlir::FailureOr<mlir::Value> MIRLowering::lowerAssign(BinaryExprAST *assign) {
  auto *target = llvm::dyn_cast<DerefExprAST>(assign->LHS.get());
  if (!target)
    return mlir::failure();
  auto value = lowerExpr(assign->RHS.get());
  if (mlir::failed(value) || !*value)
    return mlir::failure();
  auto place = lowerDerefPlace(target);
  if (mlir::failed(place))
    return mlir::failure();
  smn::StoreOp::create(builder_, loc(assign), *value, *place);
  return mlir::Value();
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
