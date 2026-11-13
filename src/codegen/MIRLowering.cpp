#include "codegen/MIRLowering.h"
#include "codegen/ScalarOps.h"
#include "util/Utilities.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/OwningOpRef.h"

//! \file MIRLowering.cpp
//! \brief A function's body and a kernel's scalar code lowered to MIR: the
//! statements, bindings and returns, the dispatch over expressions, and the
//! literals and variables. The other MIRLowering*.cpp files lower the rest,
//! each a concern of its own: calls, control flow, aggregates, places and
//! operators.

namespace sammine_lang {

using namespace AST;

namespace {

/// The bindings of a function body whose address its code takes: the
/// variable at the root of `&x`, `&x.f` or `&a[i]`, or of `x[i]` through an
/// Indexer, which lends x. By name, so a binding a later one shadows counts
/// too.
class AddressedBindings : public ASTVisitor {
public:
  explicit AddressedBindings(const ASTProperties &props) : props(props) {}
  llvm::StringSet<> names;

  void preorder_walk(AddrOfExprAST *ast) override {
    addRoot(ast->operand.get());
  }
  void preorder_walk(IndexExprAST *ast) override {
    if (auto *ip = props.index(ast->id()); ip && ip->indexer_method)
      addRoot(ast->array_expr.get());
  }

private:
  const ASTProperties &props;

  void addRoot(ExprAST *expr) {
    while (true) {
      if (auto *field = llvm::dyn_cast<FieldAccessExprAST>(expr)) {
        expr = field->object_expr.get();
      } else if (auto *index = llvm::dyn_cast<IndexExprAST>(expr);
                 index &&
                 index->array_expr->get_type().type_kind == TypeKind::Array) {
        expr = index->array_expr.get();
      } else {
        if (auto *var = llvm::dyn_cast<VariableExprAST>(expr))
          names.insert(var->variableName.mangled());
        return;
      }
    }
  }
};

} // namespace

std::optional<mlir::FunctionType>
MIRLowering::mirSignature(FuncDefAST *ast) const {
  auto type = ast->Prototype->get_type();
  if (type.type_kind != TypeKind::Function)
    return std::nullopt;
  return types_.mirFunctionType(std::get<FunctionType>(type.type_data));
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
  AddressedBindings addressed(props_);
  ast->Block->accept_vis(&addressed);
  addressed_ = std::move(addressed.names);
  for (auto [param, arg] :
       llvm::zip(ast->Prototype->parameterVectors, entry.getArguments()))
    bind(param->name, arg, loc(param.get()));

  // A body that runs off its end returns its last value, or () in a
  // function that returns nothing (FuncDefAST::body_value).
  auto value = lowerStatements(ast->Block.get());
  if (mlir::failed(value))
    return false;
  if (!terminated()) {
    if (signature->getNumResults() == 0)
      mlir::func::ReturnOp::create(builder_, loc(ast));
    else if (*value && value->getType() == signature->getResult(0))
      mlir::func::ReturnOp::create(builder_, loc(ast), *value);
    else
      return false;
  }

  (*func)->setAttr(smn::kMirAttr, builder_.getUnitAttr());
  decl->getBlock()->getOperations().insert(mlir::Block::iterator(decl),
                                           func.release());
  decl.erase();
  return true;
}

// A kernel's scalar code, as `lower` lowers it where the builder is, with
// `bindings` naming the lambda's parameters. It must be only arith ops, in
// the block it starts in: what a linalg region holds. Anything else it
// lowered is erased again.
mlir::FailureOr<mlir::Value> MIRLowering::lowerArithOnly(
    const llvm::StringMap<mlir::Value> &bindings,
    llvm::function_ref<mlir::FailureOr<mlir::Value>()> lower) {
  auto *block = builder_.getInsertionBlock();
  auto *region = block->getParent();
  auto blockCount = region->getBlocks().size();
  llvm::SmallPtrSet<mlir::Operation *, 8> before;
  for (auto &op : *block)
    before.insert(&op);
  names_ = bindings;
  addressed_.clear();

  auto value = lower();
  llvm::SmallVector<mlir::Operation *> lowered;
  for (auto &op : *block)
    if (!before.contains(&op))
      lowered.push_back(&op);
  bool arith = mlir::succeeded(value) && *value &&
               region->getBlocks().size() == blockCount &&
               llvm::all_of(lowered, [](mlir::Operation *op) {
                 return mlir::isa<mlir::arith::ArithDialect>(op->getDialect());
               });
  if (arith)
    return value;
  while (region->getBlocks().size() > blockCount)
    region->getBlocks().back().erase();
  for (auto *op : llvm::reverse(lowered))
    op->erase();
  return mlir::failure();
}

mlir::FailureOr<mlir::Value>
MIRLowering::lowerKernelScalar(BlockAST *body,
                               const llvm::StringMap<mlir::Value> &bindings) {
  return lowerArithOnly(bindings, [&]() -> mlir::FailureOr<mlir::Value> {
    mlir::Value last;
    for (auto &stmt : body->Statements) {
      auto value = lowerExpr(stmt.get());
      if (mlir::failed(value))
        return mlir::failure();
      last = *value;
    }
    return last;
  });
}

mlir::FailureOr<mlir::Value>
MIRLowering::lowerKernelScalar(ExprAST *expr,
                               const llvm::StringMap<mlir::Value> &bindings) {
  return lowerArithOnly(bindings, [&] { return lowerExpr(expr); });
}

bool MIRLowering::terminated() const {
  auto *block = builder_.getInsertionBlock();
  return !block->empty() &&
         block->back().hasTrait<mlir::OpTrait::IsTerminator>();
}

// A block's statements in order, its value the last one's. The names its
// `let`s make end with it. Once one returns, the rest never run and aren't
// lowered: the result is then a null value, and `terminated()` is true.
mlir::FailureOr<mlir::Value> MIRLowering::lowerStatements(BlockAST *block) {
  NameScope scope(*this);
  mlir::Value last;
  for (auto &stmt : block->Statements) {
    if (terminated())
      break;
    mlir::FailureOr<mlir::Value> value = mlir::Value();
    if (auto *def = llvm::dyn_cast<VarDefAST>(stmt.get()))
      value = mlir::failed(lowerVarDef(def)) ? mlir::FailureOr<mlir::Value>()
                                             : mlir::Value();
    else if (auto *ret = llvm::dyn_cast<ReturnStmtAST>(stmt.get()))
      value = mlir::failed(lowerReturn(ret)) ? mlir::FailureOr<mlir::Value>()
                                             : mlir::Value();
    else
      value = lowerExpr(stmt.get());
    if (mlir::failed(value))
      return mlir::failure();
    last = *value;
  }
  return last;
}

// A `let` of a value MIR has a type for names that value: the right side is
// lowered first, so `let x = x + 1` reads the x it shadows. `let (a, b) = t`
// names each element of t.
mlir::LogicalResult MIRLowering::lowerVarDef(VarDefAST *def) {
  // A `let (a, b) = t` is typed (); t's type is checked as it is lowered.
  if (!def->is_tuple_destructure && !types_.mirType(def->get_type()))
    return mlir::failure();
  auto value = lowerValue(def->Expression.get());
  if (mlir::failed(value))
    return mlir::failure();
  if (!def->is_tuple_destructure) {
    bind(def->TypedVar->name, *value, loc(def));
    return mlir::success();
  }
  auto tuple = mlir::dyn_cast<smn::TupleType>(value->getType());
  if (!tuple || tuple.getElements().size() != def->destructure_vars.size())
    return mlir::failure();
  for (auto [i, var] : llvm::enumerate(def->destructure_vars))
    bind(var->name,
         smn::ExtractOp::create(builder_, loc(def), tuple.getElements()[i],
                                *value, i),
         loc(def));
  return mlir::success();
}

// `name` names `value` from here on: the value itself, or, if the code
// takes its address, a slot holding it.
void MIRLowering::bind(llvm::StringRef name, mlir::Value value,
                       mlir::Location location) {
  if (!addressed_.contains(name)) {
    // A value that owns memory carries its name, for the ownership check.
    if (smn::ownsMemory(value.getType()))
      value = smn::LetOp::create(builder_, location, value.getType(),
                                 builder_.getStringAttr(name), value);
    names_[name] = value;
    return;
  }
  auto slot =
      smn::LocalOp::create(builder_, location, placeOf(value.getType()), name);
  smn::StoreOp::create(builder_, location, value, slot);
  names_[name] = slot;
}

mlir::LogicalResult MIRLowering::lowerReturn(ReturnStmtAST *ret) {
  if (!ret->return_expr)
    return mlir::failure();
  auto value = lowerExpr(ret->return_expr.get());
  if (mlir::failed(value))
    return mlir::failure();
  // What it returns already returned on every path, as `return if c { return
  // 1; } else { return 2; }`.
  if (terminated())
    return mlir::success();
  // Returning () returns nothing.
  mlir::func::ReturnOp::create(builder_, loc(ret),
                               *value ? mlir::ValueRange{*value}
                                      : mlir::ValueRange{});
  return mlir::success();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerValue(ExprAST *expr) {
  auto value = lowerExpr(expr);
  if (mlir::failed(value) || !*value)
    return mlir::failure();
  return value;
}

mlir::Value MIRLowering::intConstant(mlir::Location location, mlir::Type type,
                                     int64_t value) {
  return mlir::arith::ConstantIntOp::create(builder_, location, type, value);
}

smn::PlaceType MIRLowering::placeOf(mlir::Type type) const {
  return smn::PlaceType::get(builder_.getContext(), type);
}

smn::PtrType MIRLowering::viewOf(mlir::Type pointee) const {
  return smn::PtrType::get(builder_.getContext(), pointee,
                           smn::Ownership::View);
}

void MIRLowering::continueIn(mlir::Block *block) {
  auto *region = block->getParent();
  block->moveBefore(region, region->end());
  builder_.setInsertionPointToStart(block);
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
  if (auto *bin = llvm::dyn_cast<BinaryExprAST>(expr)) {
    if (bin->Op->is_assign())
      return lowerAssign(bin);
    if (bin->Op->is_logical())
      return lowerLogical(bin);
    return lowerBinary(bin);
  }
  if (auto *ifExpr = llvm::dyn_cast<IfExprAST>(expr))
    return lowerIf(ifExpr);
  if (auto *cast = llvm::dyn_cast<CastExprAST>(expr))
    return lowerCast(cast);
  if (auto *caseExpr = llvm::dyn_cast<CaseExprAST>(expr))
    return lowerCase(caseExpr);
  if (auto *forExpr = llvm::dyn_cast<ForExprAST>(expr))
    return lowerFor(forExpr);
  if (auto *neg = llvm::dyn_cast<UnaryNegExprAST>(expr))
    return lowerNegation(neg);
  if (auto *call = llvm::dyn_cast<CallExprAST>(expr))
    return lowerCall(call);
  if (auto *alloc = llvm::dyn_cast<AllocExprAST>(expr))
    return lowerAlloc(alloc);
  if (auto *free = llvm::dyn_cast<FreeExprAST>(expr))
    return lowerFree(free);
  if (llvm::isa<DerefExprAST>(expr))
    return lowerPlaceRead(expr);
  if (auto *index = llvm::dyn_cast<IndexExprAST>(expr)) {
    if (auto *ip = props_.index(index->id()); ip && ip->indexer_method)
      return lowerIndexerCall(index, *ip->indexer_method);
    if (hasPlace(index))
      return lowerPlaceRead(expr);
    if (index->array_expr->get_type().type_kind == TypeKind::Array)
      return lowerArrayIndex(index);
    return mlir::failure();
  }
  if (auto *addr = llvm::dyn_cast<AddrOfExprAST>(expr))
    return lowerAddrOf(addr);
  if (auto *array = llvm::dyn_cast<ArrayLiteralExprAST>(expr))
    return lowerArrayLiteral(array);
  if (auto *range = llvm::dyn_cast<RangeExprAST>(expr))
    return lowerRangeArray(range);
  if (auto *dim = llvm::dyn_cast<DimExprAST>(expr))
    return lowerDim(dim);
  if (auto *len = llvm::dyn_cast<LenExprAST>(expr))
    return lowerLen(len);
  if (auto *str = llvm::dyn_cast<StringExprAST>(expr))
    return lowerString(str);
  if (auto *lit = llvm::dyn_cast<StructLiteralExprAST>(expr))
    return lowerStructLiteral(lit);
  if (auto *tuple = llvm::dyn_cast<TupleLiteralExprAST>(expr))
    return lowerTupleLiteral(tuple);
  if (auto *access = llvm::dyn_cast<FieldAccessExprAST>(expr))
    return lowerFieldAccess(access);
  if (llvm::isa<UnitExprAST>(expr))
    return mlir::Value();
  return mlir::failure();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerNumber(NumberExprAST *num) {
  auto type = types_.mirType(num->get_type());
  if (!type)
    return mlir::failure();
  if (auto floatType = mlir::dyn_cast<mlir::FloatType>(*type))
    return emitFloatConstant(builder_, loc(num), std::stod(num->number),
                             floatType);
  return intConstant(loc(num), *type,
                     sammine_util::literal_int_bits(num->number));
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerString(StringExprAST *str) {
  auto type = types_.mirType(str->get_type());
  if (!type)
    return mlir::failure();
  return smn::StringOp::create(builder_, loc(str), *type, str->string_content)
      .getResult();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerBool(BoolExprAST *b) {
  return intConstant(loc(b), builder_.getI1Type(), b->b ? 1 : 0);
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerChar(CharExprAST *c) {
  return intConstant(loc(c), builder_.getI8Type(),
                     static_cast<uint8_t>(c->value));
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerVariable(VariableExprAST *var) {
  if (auto *vp = props_.variable(var->id()); vp && vp->is_enum_unit_variant)
    return lowerVariant(loc(var), var->get_type(), vp->enum_variant_index, {});
  auto it = names_.find(var->variableName.mangled());
  if (it == names_.end())
    return lowerFunctionValue(var);
  // A binding in a slot is read from it.
  if (mlir::isa<smn::PlaceType>(it->second.getType()))
    return read(loc(var), it->second);
  return it->second;
}

} // namespace sammine_lang
