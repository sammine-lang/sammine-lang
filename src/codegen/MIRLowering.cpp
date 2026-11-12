#include "codegen/MIRLowering.h"
#include "codegen/MLIRGenBinaryOps.h"
#include "codegen/MLIRGenImpl.h"
#include "codegen/RangeIteration.h"
#include "codegen/TypeLayout.h"
#include "typecheck/BuiltinInstances.h"
#include "dialect/SammineDialect.h"
#include "util/Utilities.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Interfaces/FunctionInterfaces.h"

//! \file MIRLowering.cpp
//! \brief The constructs MIR handles so far: functions over scalars (integers,
//! floats, bool, char), pointers, structs, tuples, arrays and enums;
//! literals, strings among them, parameters, `let`, `let (a, b) = t`;
//! arithmetic, negation, comparisons, casts, operators through instances;
//! calls to sammine and C functions, lending an owned pointer to a ptr<T>;
//! function values, partial application and calls through a function value;
//! struct, tuple, array and enum values, fields, checked `a[i]`, `len`,
//! `case`; `alloc`, `free`, `*p` and
//! `p[i]` read and written; `if`, `for`, `&&` and `||`, statements and
//! return.

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
                 index && index->array_expr->get_type().type_kind ==
                              TypeKind::Array) {
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
    bind(param->name, arg, loc(ast));

  // GeneralSemanticsVisitor makes a body's implicit return explicit, but
  // not in a function that writes `-> unit`: one that runs off its end
  // returns (), or its last value.
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
    names_[name] = value;
    return;
  }
  auto slot = smn::LocalOp::create(
      builder_, location,
      placeOf(value.getType()), name);
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

// `f(a, b)`, `f` a sammine function, a generic function's copy, an
// instance's method or a C function.
mlir::FailureOr<mlir::Value> MIRLowering::lowerCall(CallExprAST *call) {
  auto *cp = props_.call(call->id());
  if (cp->is_enum_constructor) {
    llvm::SmallVector<ExprAST *> payload;
    for (auto &arg : call->arguments)
      payload.push_back(arg.get());
    return lowerVariant(loc(call), call->get_type(), cp->enum_variant_index,
                        payload);
  }
  if (cp->builtin_instance) {
    llvm::SmallVector<mlir::Value, 2> args;
    for (auto &arg : call->arguments) {
      auto value = lowerValue(arg.get());
      if (mlir::failed(value))
        return mlir::failure();
      args.push_back(*value);
    }
    return lowerBuiltinInstance(loc(call), *cp->builtin_instance, args,
                                call->get_type());
  }
  auto name =
      props_.symbol(cp->resolved_name ? *cp->resolved_name : call->functionName);
  // A name no function has is a binding holding a function value.
  auto callee = module_.lookupSymbol<mlir::FunctionOpInterface>(name);
  if (!callee)
    return lowerIndirectCall(call);
  auto type =
      types_.mirFunctionType(std::get<FunctionType>(cp->callee_func_type->type_data));
  if (!type)
    return mlir::failure();
  if (cp->is_partial)
    return lowerPartialApplication(call, name, *type);

  // The arguments for the declared parameters, then any passed through a C
  // function's `...`.
  auto params = type->getInputs();
  llvm::SmallVector<mlir::Value, 4> args;
  for (auto [i, arg] : llvm::enumerate(call->arguments)) {
    auto value = i < params.size() ? lowerArgument(arg.get(), params[i])
                                   : lowerVariadicArgument(arg.get());
    if (mlir::failed(value))
      return mlir::failure();
    args.push_back(*value);
  }
  auto op = smn::CallOp::create(builder_, loc(call), type->getResults(), name,
                                args);
  return op.getNumResults() ? op.getResult(0) : mlir::Value();
}

// `f(a)`, f a binding holding a function value: a call through it.
mlir::FailureOr<mlir::Value> MIRLowering::lowerIndirectCall(CallExprAST *call) {
  auto it = names_.find(call->functionName.mangled());
  if (it == names_.end())
    return mlir::failure();
  mlir::Value callee = it->second;
  if (mlir::isa<smn::PlaceType>(callee.getType()))
    callee = read(loc(call), callee);
  auto fn = mlir::dyn_cast<smn::FnType>(callee.getType());
  if (!fn || fn.getSignature().getNumInputs() != call->arguments.size())
    return mlir::failure();
  llvm::SmallVector<mlir::Value, 4> args;
  for (auto [arg, param] :
       llvm::zip(call->arguments, fn.getSignature().getInputs())) {
    auto value = lowerArgument(arg.get(), param);
    if (mlir::failed(value))
      return mlir::failure();
    args.push_back(*value);
  }
  auto op = smn::CallIndirectOp::create(
      builder_, loc(call), fn.getSignature().getResults(), callee, args);
  return op.getNumResults() ? op.getResult(0) : mlir::Value();
}

// `add(1)`, giving fewer arguments than add takes: a function value of add
// with them bound, which takes the rest.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerPartialApplication(CallExprAST *call, llvm::StringRef name,
                                     mlir::FunctionType type) {
  auto result = types_.mirType(call->get_type());
  if (!result || !mlir::isa<smn::FnType>(*result) ||
      call->arguments.size() > type.getNumInputs())
    return mlir::failure();
  llvm::SmallVector<mlir::Value, 4> bound;
  for (auto [arg, param] : llvm::zip(call->arguments, type.getInputs())) {
    auto value = lowerArgument(arg.get(), param, ViewLasts::Place);
    if (mlir::failed(value))
      return mlir::failure();
    bound.push_back(*value);
  }
  return smn::ClosureOp::create(builder_, loc(call), *result, name, bound)
      .getResult();
}

// An argument for a parameter of type `param`. An owned pointer passed to a
// ptr<T> is lent: the callee gets a view of what it points at, and the caller
// keeps it. `view` says how long the view lasts: the call, or, for an
// argument a partial application binds, as long as what it points at.
mlir::FailureOr<mlir::Value> MIRLowering::lowerArgument(ExprAST *arg,
                                                        mlir::Type param,
                                                        ViewLasts view) {
  auto value = lowerValue(arg);
  if (mlir::failed(value))
    return mlir::failure();
  if (value->getType() == param)
    return *value;
  auto ptr = mlir::dyn_cast<smn::PtrType>(value->getType());
  if (!ptr || !ptr.isOwned() ||
      param != viewOf(ptr.getPointee()))
    return mlir::failure();
  return lend(*value, loc(arg), view);
}

// An argument passed through a C function's `...`. Nothing declares its
// type, and C only reads what sammine passes it (the type checker keeps
// addresses out), so an owned pointer is lent, as the linear checker has it.
mlir::FailureOr<mlir::Value> MIRLowering::lowerVariadicArgument(ExprAST *arg) {
  auto value = lowerValue(arg);
  if (mlir::failed(value))
    return mlir::failure();
  if (smn::ownsMemory(value->getType()))
    return lend(*value, loc(arg), ViewLasts::Call);
  return *value;
}

mlir::Value MIRLowering::lend(mlir::Value owner, mlir::Location loc,
                              ViewLasts view) {
  auto pointee = mlir::cast<smn::PtrType>(owner.getType()).getPointee();
  auto place = smn::DerefOp::create(builder_, loc,
                                    placeOf(pointee), owner);
  auto type = viewOf(pointee);
  if (view == ViewLasts::Place)
    return smn::AddrOp::create(builder_, loc, type, place).getResult();
  return smn::LendOp::create(builder_, loc, type, place).getResult();
}

// The type an if's or a case's value has in MIR: none if it is () or never
// reached; failure if MIR has no type for it.
mlir::FailureOr<std::optional<mlir::Type>>
MIRLowering::branchResultType(const Type &type) const {
  if (type.type_kind == TypeKind::Unit || type.type_kind == TypeKind::Never)
    return std::optional<mlir::Type>();
  auto mir = types_.mirType(type);
  if (!mir)
    return mlir::failure();
  return mir;
}

// Each arm in its block: what its pattern binds, then its body. The arms
// that don't return go on to a block after, which takes their value, of
// `resultType`. There is none if every arm returns.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerArms(mlir::Location location,
                       std::optional<mlir::Type> resultType,
                       llvm::ArrayRef<Arm> arms) {
  auto *region = arms.front().block->getParent();
  auto *after = &region->emplaceBlock();
  if (resultType)
    after->addArgument(*resultType, location);
  bool reached = false;
  for (auto &arm : arms) {
    builder_.setInsertionPointToStart(arm.block);
    mlir::FailureOr<mlir::Value> value;
    {
      NameScope scope(*this);
      if (arm.bind)
        arm.bind();
      value = lowerStatements(arm.body);
    }
    if (mlir::failed(value))
      return mlir::failure();
    if (terminated())
      continue;
    if (resultType && !*value)
      return mlir::failure();
    mlir::cf::BranchOp::create(builder_, location, after,
                               resultType ? mlir::ValueRange{*value}
                                          : mlir::ValueRange{});
    reached = true;
  }
  if (!reached) {
    after->erase();
    return mlir::Value();
  }
  // After the blocks the arms made.
  continueIn(after);
  return resultType ? after->getArgument(0) : mlir::Value();
}

// `if c { a } else { b }`: c branches to a block for each side.
mlir::FailureOr<mlir::Value> MIRLowering::lowerIf(IfExprAST *ifExpr) {
  auto resultType = branchResultType(ifExpr->get_type());
  if (mlir::failed(resultType))
    return mlir::failure();
  auto cond = lowerValue(ifExpr->bool_expr.get());
  if (mlir::failed(cond))
    return mlir::failure();
  auto *region = builder_.getInsertionBlock()->getParent();
  auto *thenBlock = &region->emplaceBlock();
  auto *elseBlock = &region->emplaceBlock();
  mlir::cf::CondBranchOp::create(builder_, loc(ifExpr), *cond, thenBlock,
                                 elseBlock);
  return lowerArms(loc(ifExpr), *resultType,
                   {Arm{thenBlock, ifExpr->thenBlockAST.get(), nullptr},
                    Arm{elseBlock, ifExpr->elseBlockAST.get(), nullptr}});
}

// `case e { Some(x) => a, None => b }` on an enum: the tag picks the arm's
// block, where the pattern's names are bound to the payload. With no
// wildcard, any other tag goes to the last arm: the arms cover every
// variant.
mlir::FailureOr<mlir::Value> MIRLowering::lowerCase(CaseExprAST *caseExpr) {
  auto scrutineeType = caseExpr->scrutinee->get_type();
  if (caseExpr->arms.empty())
    return mlir::failure();
  if (scrutineeType.type_kind != TypeKind::Enum ||
      std::get<EnumType>(scrutineeType.type_data).is_integer_backed())
    return lowerScalarCase(caseExpr);
  auto et = std::get<EnumType>(scrutineeType.type_data);
  auto resultType = branchResultType(caseExpr->get_type());
  if (mlir::failed(resultType))
    return mlir::failure();
  auto value = lowerValue(caseExpr->scrutinee.get());
  if (mlir::failed(value))
    return mlir::failure();
  auto enumType = mlir::dyn_cast<smn::EnumType>(value->getType());
  if (!enumType)
    return mlir::failure();

  auto location = loc(caseExpr);
  auto tag =
      smn::TagOp::create(builder_, location, builder_.getI32Type(), *value);
  auto *region = builder_.getInsertionBlock()->getParent();
  llvm::SmallVector<Arm> arms;
  llvm::SmallVector<int32_t> tags;
  llvm::SmallVector<mlir::Block *> dests;
  mlir::Block *otherwise = nullptr;
  for (auto &arm : caseExpr->arms) {
    auto &pattern = arm.pattern;
    if (pattern.is_literal)
      return mlir::failure();
    auto *block = &region->emplaceBlock();
    if (pattern.is_wildcard) {
      otherwise = block;
      arms.push_back(Arm{block, arm.body.get(), nullptr});
      continue;
    }
    tags.push_back(static_cast<int32_t>(pattern.variant_index));
    dests.push_back(block);
    auto &payloadTypes = et.get_variant(pattern.variant_index).payload_types;
    auto mirPayload = enumType.getPayload(pattern.variant_index);
    if (pattern.bindings.size() > payloadTypes.size())
      return mlir::failure();
    // Run by lowerArms, after this loop: it holds only what outlives it.
    arms.push_back(Arm{block, arm.body.get(),
                       [this, &pattern, &payloadTypes, mirPayload,
                        scrutinee = *value, location] {
      int64_t offset = 0;
      for (auto [i, name] : llvm::enumerate(pattern.bindings)) {
        auto at = layout::advancePayloadOffset(offset, payloadTypes[i]);
        bind(name,
             smn::PayloadOp::create(builder_, location, mirPayload[i],
                                    scrutinee, pattern.variant_index, i,
                                    static_cast<uint64_t>(at)),
             location);
      }
    }});
  }
  if (!otherwise) {
    otherwise = dests.pop_back_val();
    tags.pop_back();
  }
  branchOn(location, tag, tags, dests, otherwise, mlir::ValueRange{});
  return lowerArms(location, *resultType, arms);
}

// `for i in a..b { body }`: Range { a, b } goes
// round a loop, each turn calling Iterator<Range<T>, T>::next on it. Some
// gives the next range and i, for the body, then the loop goes round with
// that range; None leaves the loop.
mlir::FailureOr<mlir::Value> MIRLowering::lowerFor(ForExprAST *forExpr) {
  auto *forProps = props_.for_loop(forExpr->id());
  if (!forProps || !forProps->range_type)
    return mlir::failure();
  auto elementType = forExpr->start->get_type();
  auto iteration = rangeIteration(*forProps->range_type, elementType);
  auto rangeType = types_.mirType(*forProps->range_type);
  auto optionType = types_.mirType(iteration.option);
  auto stepType = types_.mirType(iteration.step);
  auto element = types_.mirType(elementType);
  if (!rangeType || !optionType || !stepType || !element ||
      !module_.lookupSymbol<mlir::FunctionOpInterface>(iteration.next))
    return mlir::failure();
  auto location = loc(forExpr);

  auto start = lowerValue(forExpr->start.get());
  if (mlir::failed(start))
    return mlir::failure();
  auto end = lowerValue(forExpr->end.get());
  if (mlir::failed(end))
    return mlir::failure();
  auto range = smn::AggregateOp::create(builder_, location, *rangeType,
                                        mlir::ValueRange{*start, *end});

  auto *region = builder_.getInsertionBlock()->getParent();
  auto *header = &region->emplaceBlock();
  header->addArgument(*rangeType, location);
  mlir::cf::BranchOp::create(builder_, location, header,
                             mlir::ValueRange{range});

  builder_.setInsertionPointToStart(header);
  auto option = smn::CallOp::create(builder_, location,
                                    mlir::TypeRange{*optionType},
                                    iteration.next, header->getArgument(0))
                    .getResult(0);
  auto tag = smn::TagOp::create(builder_, location, builder_.getI32Type(),
                                option);
  auto some = intConstant(location,
                                                 builder_.getI32Type(), 0);
  auto isSome = mlir::arith::CmpIOp::create(
      builder_, location, mlir::arith::CmpIPredicate::eq, tag, some);
  auto *body = &region->emplaceBlock();
  auto *exit = &region->emplaceBlock();
  mlir::cf::CondBranchOp::create(builder_, location, isSome, body, exit);

  builder_.setInsertionPointToStart(body);
  auto step = smn::PayloadOp::create(builder_, location, *stepType, option,
                                     /*variant=*/0, /*field=*/0, /*offset=*/0);
  auto next = smn::ExtractOp::create(builder_, location, *rangeType, step, 0);
  mlir::FailureOr<mlir::Value> value;
  {
    NameScope scope(*this);
    bind(forExpr->loop_var,
         smn::ExtractOp::create(builder_, location, *element, step, 1),
         location);
    value = lowerStatements(forExpr->body.get());
  }
  if (mlir::failed(value))
    return mlir::failure();
  if (!terminated())
    mlir::cf::BranchOp::create(builder_, location, header,
                               mlir::ValueRange{next});

  continueIn(exit);
  return mlir::Value();
}

// `case n { 0 => a, 1 => b, _ => c }` on an integer, char or bool, or on an
// integer-backed enum's variants: a cf.switch on the value, to each arm's
// block. With no wildcard, any other value goes to the last arm: the arms
// cover them all. An arm for a value an earlier arm took is never reached.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerScalarCase(CaseExprAST *caseExpr) {
  auto scrutineeType = caseExpr->scrutinee->get_type();
  std::optional<EnumType> intEnum;
  if (scrutineeType.type_kind == TypeKind::Enum)
    intEnum = std::get<EnumType>(scrutineeType.type_data);
  auto resultType = branchResultType(caseExpr->get_type());
  if (mlir::failed(resultType))
    return mlir::failure();
  auto value = lowerValue(caseExpr->scrutinee.get());
  if (mlir::failed(value))
    return mlir::failure();
  auto flagType = mlir::dyn_cast<mlir::IntegerType>(value->getType());
  if (!flagType)
    return mlir::failure();

  // The value each arm matches, as its pattern says it.
  using LK = CasePattern::LiteralKind;
  auto armValue = [&](const CasePattern &pattern) -> std::optional<int64_t> {
    if (intEnum)
      return intEnum->get_variant(pattern.variant_index).discriminant_value;
    switch (pattern.literal_kind) {
    case LK::Bool:
      return pattern.literal_value == "true" ? 1 : 0;
    case LK::Char:
      return static_cast<uint8_t>(pattern.literal_value[0]);
    case LK::Integer:
      return sammine_util::literal_int_bits(pattern.literal_value);
    default:
      return std::nullopt;
    }
  };

  auto *region = builder_.getInsertionBlock()->getParent();
  llvm::SmallVector<Arm> arms;
  llvm::SmallVector<llvm::APInt> values;
  llvm::SmallVector<mlir::Block *> dests;
  mlir::Block *otherwise = nullptr;
  for (auto &arm : caseExpr->arms) {
    auto *block = &region->emplaceBlock();
    arms.push_back(Arm{block, arm.body.get(), nullptr});
    if (arm.pattern.is_wildcard) {
      otherwise = block;
      break;
    }
    if (!intEnum && !arm.pattern.is_literal)
      return mlir::failure();
    auto matched = armValue(arm.pattern);
    if (!matched)
      return mlir::failure();
    llvm::APInt bits(flagType.getWidth(), static_cast<uint64_t>(*matched),
                     /*isSigned=*/true, /*implicitTrunc=*/true);
    if (llvm::is_contained(values, bits))
      continue;
    values.push_back(bits);
    dests.push_back(block);
  }
  if (!otherwise) {
    if (dests.empty())
      return mlir::failure();
    otherwise = dests.pop_back_val();
    values.pop_back();
  }
  auto location = loc(caseExpr);
  branchOn(location, *value, values, dests, otherwise, mlir::ValueRange{});
  return lowerArms(location, *resultType, arms);
}

// Branches on `flag`: to the block of the value it equals, else to
// `otherwise`, passed `otherwiseArgs`. With no values that is a plain
// branch: MLIR's translation to LLVM IR can't take a switch with no cases
// whose default takes arguments (upstream/mlir-switch-no-cases-phi).
void MIRLowering::branchOn(mlir::Location location, mlir::Value flag,
                           llvm::ArrayRef<llvm::APInt> values,
                           llvm::ArrayRef<mlir::Block *> dests,
                           mlir::Block *otherwise,
                           mlir::ValueRange otherwiseArgs) {
  if (values.empty()) {
    mlir::cf::BranchOp::create(builder_, location, otherwise, otherwiseArgs);
    return;
  }
  auto caseValues = mlir::DenseIntElementsAttr::get(
      mlir::VectorType::get({static_cast<int64_t>(values.size())},
                            flag.getType()),
      values);
  llvm::SmallVector<mlir::ValueRange> noOperands(dests.size());
  mlir::cf::SwitchOp::create(builder_, location, flag, otherwiseArgs,
                             noOperands, caseValues, otherwise, dests);
}

void MIRLowering::branchOn(mlir::Location location, mlir::Value flag,
                           llvm::ArrayRef<int32_t> tags,
                           llvm::ArrayRef<mlir::Block *> dests,
                           mlir::Block *otherwise,
                           mlir::ValueRange otherwiseArgs) {
  llvm::SmallVector<llvm::APInt> values;
  for (auto tag : tags)
    values.emplace_back(32, static_cast<uint64_t>(tag), /*isSigned=*/true);
  branchOn(location, flag, values, dests, otherwise, otherwiseArgs);
}

// `Option<i64>::Some(x)`, or a variant with no payload, `None`: its payload
// fields at the offsets layout:: gives them.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerVariant(mlir::Location location, const Type &type,
                          size_t index, llvm::ArrayRef<ExprAST *> payload) {
  auto mir = types_.mirType(type);
  if (!mir)
    return mlir::failure();
  // A variant of an integer-backed enum is its discriminant.
  if (auto backing = mlir::dyn_cast<mlir::IntegerType>(*mir)) {
    auto discriminant =
        std::get<EnumType>(type.type_data).get_variant(index).discriminant_value;
    if (!discriminant || !payload.empty())
      return mlir::failure();
    return intConstant(location, backing,
                                              *discriminant);
  }
  auto enumType = mlir::cast<smn::EnumType>(*mir);
  auto &payloadTypes =
      std::get<EnumType>(type.type_data).get_variant(index).payload_types;
  if (payload.size() != payloadTypes.size())
    return mlir::failure();
  llvm::SmallVector<mlir::Value> fields;
  llvm::SmallVector<int64_t> offsets;
  int64_t offset = 0;
  for (auto [expr, fieldType, mirField] :
       llvm::zip(payload, payloadTypes, enumType.getPayload(index))) {
    auto value = lowerValue(expr);
    if (mlir::failed(value) || value->getType() != mirField)
      return mlir::failure();
    fields.push_back(*value);
    offsets.push_back(layout::advancePayloadOffset(offset, fieldType));
  }
  return smn::VariantOp::create(builder_, location, enumType, index, offsets,
                                fields)
      .getResult();
}

// `a && b` reads b only if a is true, `a || b` only if a is false; the other
// way, the result is a.
mlir::FailureOr<mlir::Value> MIRLowering::lowerLogical(BinaryExprAST *bin) {
  auto lhs = lowerValue(bin->LHS.get());
  if (mlir::failed(lhs))
    return mlir::failure();
  bool isAnd = bin->Op->tok_type == TokAND;
  auto location = loc(bin);
  auto shortCut = intConstant(location, builder_.getI1Type(), isAnd ? 0 : 1);

  auto *region = builder_.getInsertionBlock()->getParent();
  auto *rhsBlock = &region->emplaceBlock();
  auto *after = &region->emplaceBlock();
  after->addArgument(builder_.getI1Type(), location);
  if (isAnd)
    mlir::cf::CondBranchOp::create(builder_, location, *lhs, rhsBlock,
                                   mlir::ValueRange{}, after,
                                   mlir::ValueRange{shortCut});
  else
    mlir::cf::CondBranchOp::create(builder_, location, *lhs, after,
                                   mlir::ValueRange{shortCut}, rhsBlock,
                                   mlir::ValueRange{});

  builder_.setInsertionPointToStart(rhsBlock);
  auto rhs = lowerValue(bin->RHS.get());
  if (mlir::failed(rhs) || terminated())
    return mlir::failure();
  mlir::cf::BranchOp::create(builder_, location, after,
                             mlir::ValueRange{*rhs});
  continueIn(after);
  return after->getArgument(0);
}

// The values of `exprs`, in order, as the fields of an aggregate of `type`.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerAggregate(mlir::Location location, mlir::Type type,
                            llvm::ArrayRef<mlir::Type> fieldTypes,
                            llvm::ArrayRef<std::pair<size_t, ExprAST *>> exprs) {
  llvm::SmallVector<mlir::Value> fields(fieldTypes.size());
  for (auto [index, expr] : exprs) {
    auto value = lowerValue(expr);
    if (mlir::failed(value) || value->getType() != fieldTypes[index])
      return mlir::failure();
    fields[index] = *value;
  }
  if (llvm::is_contained(fields, mlir::Value()))
    return mlir::failure();
  return smn::AggregateOp::create(builder_, location, type, fields).getResult();
}

// `Point { y: b, x: a }`: the fields' values in the order written, each put
// at its field's place.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerStructLiteral(StructLiteralExprAST *lit) {
  auto type = types_.mirType(lit->get_type());
  if (!type)
    return mlir::failure();
  auto st = std::get<StructType>(lit->get_type().type_data);
  llvm::SmallVector<std::pair<size_t, ExprAST *>> exprs;
  for (auto [name, value] : llvm::zip(lit->field_names, lit->field_values)) {
    auto index = st.get_field_index(name);
    if (!index)
      return mlir::failure();
    exprs.push_back({*index, value.get()});
  }
  return lowerAggregate(loc(lit), *type,
                        mlir::cast<smn::StructType>(*type).getFields(), exprs);
}

mlir::FailureOr<mlir::Value>
MIRLowering::lowerTupleLiteral(TupleLiteralExprAST *tuple) {
  auto type = types_.mirType(tuple->get_type());
  if (!type)
    return mlir::failure();
  llvm::SmallVector<std::pair<size_t, ExprAST *>> exprs;
  for (auto [i, element] : llvm::enumerate(tuple->elements))
    exprs.push_back({i, element.get()});
  return lowerAggregate(loc(tuple), *type,
                        mlir::cast<smn::TupleType>(*type).getElements(), exprs);
}

// A function named as a value, `apply(f, x)`: f binding nothing.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerFunctionValue(VariableExprAST *var) {
  auto type = types_.mirType(var->get_type());
  auto name = props_.symbol(var->variableName);
  if (!type || !mlir::isa<smn::FnType>(*type) ||
      !module_.lookupSymbol<mlir::FunctionOpInterface>(name))
    return mlir::failure();
  return smn::ClosureOp::create(builder_, loc(var), *type, name,
                                mlir::ValueRange{})
      .getResult();
}

// `p.x`, or `t.0` of a tuple: read from its place when it has one, so only
// the field is copied or moved, else taken from the aggregate's value.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerFieldAccess(FieldAccessExprAST *access) {
  if (hasPlace(access))
    return lowerPlaceRead(access);
  auto index = fieldIndex(access);
  auto type = types_.mirType(access->get_type());
  if (!index || !type)
    return mlir::failure();
  auto object = lowerValue(access->object_expr.get());
  if (mlir::failed(object))
    return mlir::failure();
  return smn::ExtractOp::create(builder_, loc(access), *type, *object, *index)
      .getResult();
}

// A call to `method`, a method of an instance the program declares, chosen
// by the type checker: an operator, or `as` through Convert. Its arguments
// are the operands, by value.
mlir::FailureOr<mlir::Value>
MIRLowering::callMethod(mlir::Location location,
                        const sammine_util::QualifiedName &method,
                        mlir::ValueRange args, const Type &result) {
  auto type = types_.mirType(result);
  auto name = props_.symbol(method);
  if (!type || !module_.lookupSymbol<mlir::func::FuncOp>(name))
    return mlir::failure();
  return smn::CallOp::create(builder_, location, mlir::TypeRange{*type}, name,
                             args)
      .getResult(0);
}

// An instance the compiler provides has no function to call: Convert<A, B>
// is a number conversion, an operator's class the operator on scalars.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerBuiltinInstance(mlir::Location location,
                                  const MonomorphizedKey &key,
                                  mlir::ValueRange args, const Type &result) {
  if (key.name == kConvertClass) {
    auto to = types_.mirType(key.type_args[1]);
    if (!to || args.size() != 1)
      return mlir::failure();
    return emitNumberConversion(builder_, location, args[0], *to,
                                key.type_args[0].is_unsigned(),
                                key.type_args[1].is_unsigned());
  }
  auto op = operator_of_class(key.name);
  if (!op || args.size() != 2)
    return mlir::failure();
  return lowerOperator(location, *op, args[0], args[1], key.type_args[0]);
}

// `[a, b, c]`: its elements' values, in order.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerArrayLiteral(ArrayLiteralExprAST *array) {
  auto type = types_.mirType(array->get_type());
  if (!type)
    return mlir::failure();
  auto arrayType = mlir::cast<smn::ArrayType>(*type);
  if (static_cast<size_t>(arrayType.getSize()) != array->elements.size())
    return mlir::failure();
  llvm::SmallVector<mlir::Type> elementTypes(array->elements.size(),
                                             arrayType.getElement());
  llvm::SmallVector<std::pair<size_t, ExprAST *>> exprs;
  for (auto [i, element] : llvm::enumerate(array->elements))
    exprs.push_back({i, element.get()});
  return lowerAggregate(loc(array), *type, elementTypes, exprs);
}

// `[a...b]`, an array of a, a + 1, ...: filled in a slot by a loop, then
// read whole.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerRangeArray(RangeExprAST *range) {
  auto type = types_.mirType(range->get_type());
  if (!type)
    return mlir::failure();
  auto array = mlir::cast<smn::ArrayType>(*type);
  auto element = mlir::dyn_cast<mlir::IntegerType>(array.getElement());
  if (!element)
    return mlir::failure();
  auto start = lowerValue(range->start.get());
  if (mlir::failed(start) || start->getType() != element)
    return mlir::failure();
  auto location = loc(range);
  auto slot = smn::LocalOp::create(builder_, location,
                                   placeOf(array), "");

  auto *region = builder_.getInsertionBlock()->getParent();
  auto *header = &region->emplaceBlock();
  header->addArgument(element, location);
  auto *body = &region->emplaceBlock();
  auto *after = &region->emplaceBlock();
  mlir::cf::BranchOp::create(builder_, location, header,
                             mlir::ValueRange{intConstant(location, element, 0)});
  builder_.setInsertionPointToStart(header);
  auto index = header->getArgument(0);
  auto more = mlir::arith::CmpIOp::create(builder_, location,
                                          mlir::arith::CmpIPredicate::slt,
                                          index, intConstant(location, element, array.getSize()));
  mlir::cf::CondBranchOp::create(builder_, location, more, body, after);
  builder_.setInsertionPointToStart(body);
  auto value = mlir::arith::AddIOp::create(builder_, location, *start, index);
  auto at = smn::ElemOp::create(builder_, location,
                                placeOf(element), slot, index);
  smn::StoreOp::create(builder_, location, value, at);
  auto next = mlir::arith::AddIOp::create(builder_, location, index,
                                          intConstant(location, element, 1));
  mlir::cf::BranchOp::create(builder_, location, header,
                             mlir::ValueRange{next});
  continueIn(after);
  return read(location, slot);
}

// `dim(a)`: the sizes of a nested array's dimensions, a tuple of i32
// constants, from its type alone.
mlir::FailureOr<mlir::Value> MIRLowering::lowerDim(DimExprAST *dim) {
  auto type = types_.mirType(dim->get_type());
  if (!type)
    return mlir::failure();
  llvm::SmallVector<mlir::Value> sizes;
  for (Type current = dim->operand->get_type();
       current.type_kind == TypeKind::Array;
       current = std::get<ArrayType>(current.type_data).get_element())
    sizes.push_back(intConstant(loc(dim), builder_.getI32Type(),
        static_cast<int64_t>(
            std::get<ArrayType>(current.type_data).get_size())));
  return smn::AggregateOp::create(builder_, loc(dim), *type, sizes)
      .getResult();
}

// `a[i]` of an array: i is checked first, and one out of bounds reported on
// stderr before the program exits.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerArrayIndex(IndexExprAST *index) {
  auto array = lowerValue(index->array_expr.get());
  if (mlir::failed(array))
    return mlir::failure();
  auto offset = lowerValue(index->index_expr.get());
  if (mlir::failed(offset))
    return mlir::failure();
  auto arrayType = mlir::dyn_cast<smn::ArrayType>(array->getType());
  if (!arrayType || !mlir::isa<mlir::IntegerType>(offset->getType()))
    return mlir::failure();
  if (mlir::failed(checkBounds(loc(index), *offset, arrayType.getSize())))
    return mlir::failure();
  return smn::ElementOp::create(builder_, loc(index), arrayType.getElement(),
                                *array, *offset)
      .getResult();
}

// Whether `offset` is within an array of `size` elements, compared as i64 so
// that a wide index can't wrap into range. Out of it, the index and the
// size go to stderr through dprintf, then exit(1); in it, lowering goes on
// in a block of its own.
mlir::LogicalResult MIRLowering::checkBounds(mlir::Location location,
                                             mlir::Value offset,
                                             int64_t size) {
  if (!module_.lookupSymbol(kDprintfFunc) || !module_.lookupSymbol(kExitFunc))
    return mlir::failure();
  auto i32 = builder_.getI32Type();
  auto i64 = builder_.getI64Type();
  mlir::Value wide = offset;
  if (offset.getType() != i64)
    wide = mlir::arith::ExtSIOp::create(builder_, location, i64, offset);
  auto sizeValue = intConstant(location, i64,
                                                      size);
  auto zero = intConstant(location, i64, 0);
  auto high = mlir::arith::CmpIOp::create(
      builder_, location, mlir::arith::CmpIPredicate::sge, wide, sizeValue);
  auto low = mlir::arith::CmpIOp::create(
      builder_, location, mlir::arith::CmpIPredicate::slt, wide, zero);
  auto outside = mlir::arith::OrIOp::create(builder_, location, high, low);

  auto *region = builder_.getInsertionBlock()->getParent();
  auto *fail = &region->emplaceBlock();
  auto *inside = &region->emplaceBlock();
  mlir::cf::CondBranchOp::create(builder_, location, outside, fail, inside);

  builder_.setInsertionPointToStart(fail);
  auto message = smn::StringOp::create(
      builder_, location,
      viewOf(builder_.getI8Type()),
      "index %lld out of bounds for an array of length %lld\n");
  auto stderrFd =
      intConstant(location, i32, 2);
  smn::CallOp::create(builder_, location, mlir::TypeRange{i32}, kDprintfFunc,
                      mlir::ValueRange{stderrFd, message, wide, sizeValue});
  auto one = intConstant(location, i32, 1);
  smn::CallOp::create(builder_, location, mlir::TypeRange{}, kExitFunc,
                      mlir::ValueRange{one});
  smn::UnreachableOp::create(builder_, location);

  builder_.setInsertionPointToStart(inside);
  return mlir::success();
}

// `x as T`, through the Convert<A, T> instance the type checker chose.
mlir::FailureOr<mlir::Value> MIRLowering::lowerCast(CastExprAST *cast) {
  auto *cp = props_.cast(cast->id());
  if (!cp || !cp->instance)
    return mlir::failure();
  auto value = lowerValue(cast->operand.get());
  if (mlir::failed(value))
    return mlir::failure();
  auto &key = *cp->instance;
  if (is_builtin_instance(key))
    return lowerBuiltinInstance(loc(cast), key, *value, cast->get_type());
  return callMethod(loc(cast),
                    key.to_typeclass_name(std::string(kConvertClass)), *value,
                    cast->get_type());
}

// alloc<T>(n): an owned pointer to room for n values of T, none stored yet.
mlir::FailureOr<mlir::Value> MIRLowering::lowerAlloc(AllocExprAST *alloc) {
  auto type = types_.mirType(alloc->get_type());
  auto count = lowerValue(alloc->operand.get());
  if (!type || mlir::failed(count))
    return mlir::failure();
  return smn::AllocOp::create(builder_, loc(alloc), *type, *count).getResult();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerFree(FreeExprAST *free) {
  auto ptr = lowerValue(free->operand.get());
  if (mlir::failed(ptr))
    return mlir::failure();
  smn::FreeOp::create(builder_, loc(free), *ptr);
  return mlir::Value();
}

// The place `*p` is, for `p` a pointer MIR has a type for.
mlir::FailureOr<mlir::Value> MIRLowering::lowerDerefPlace(DerefExprAST *deref) {
  if (!types_.mirType(deref->operand->get_type()))
    return mlir::failure();
  auto ptr = lowerValue(deref->operand.get());
  if (mlir::failed(ptr))
    return mlir::failure();
  auto pointee = mlir::cast<smn::PtrType>(ptr->getType()).getPointee();
  return smn::DerefOp::create(builder_, loc(deref),
                              placeOf(pointee),
                              *ptr)
      .getResult();
}

// Reading `*p` or `p[i]`, for p a pointer.
mlir::FailureOr<mlir::Value> MIRLowering::lowerPlaceRead(ExprAST *expr) {
  auto place = lowerPlace(expr);
  if (mlir::failed(place))
    return mlir::failure();
  return read(loc(expr), *place);
}

// Whether `expr` names a place: a binding in a slot, what a pointer points
// at, a field or array element of a place, or an element past a pointer.
// Lowers nothing.
bool MIRLowering::hasPlace(ExprAST *expr) const {
  if (auto *var = llvm::dyn_cast<VariableExprAST>(expr)) {
    auto it = names_.find(var->variableName.mangled());
    return it != names_.end() &&
           mlir::isa<smn::PlaceType>(it->second.getType());
  }
  if (llvm::isa<DerefExprAST>(expr))
    return true;
  if (auto *field = llvm::dyn_cast<FieldAccessExprAST>(expr))
    return hasPlace(field->object_expr.get());
  if (auto *index = llvm::dyn_cast<IndexExprAST>(expr)) {
    if (auto *ip = props_.index(index->id()); ip && ip->indexer_method)
      return false;
    auto kind = index->array_expr->get_type().type_kind;
    return kind == TypeKind::Pointer ||
           (kind == TypeKind::Array && hasPlace(index->array_expr.get()));
  }
  return false;
}

// The place `target` names, which hasPlace says it has.
mlir::FailureOr<mlir::Value> MIRLowering::lowerPlace(ExprAST *target) {
  if (auto *var = llvm::dyn_cast<VariableExprAST>(target)) {
    auto it = names_.find(var->variableName.mangled());
    if (it == names_.end() || !mlir::isa<smn::PlaceType>(it->second.getType()))
      return mlir::failure();
    return it->second;
  }
  if (auto *deref = llvm::dyn_cast<DerefExprAST>(target))
    return lowerDerefPlace(deref);
  if (auto *field = llvm::dyn_cast<FieldAccessExprAST>(target)) {
    auto base = lowerPlace(field->object_expr.get());
    if (mlir::failed(base))
      return mlir::failure();
    auto index = fieldIndex(field);
    auto type = types_.mirType(field->get_type());
    if (!index || !type)
      return mlir::failure();
    return smn::FieldOp::create(
               builder_, loc(field),
               placeOf(*type), *base, *index)
        .getResult();
  }
  if (auto *index = llvm::dyn_cast<IndexExprAST>(target)) {
    if (index->array_expr->get_type().type_kind == TypeKind::Pointer)
      return lowerPtrElemPlace(index);
    auto base = lowerPlace(index->array_expr.get());
    if (mlir::failed(base))
      return mlir::failure();
    auto array = mlir::dyn_cast<smn::ArrayType>(
        mlir::cast<smn::PlaceType>(base->getType()).getValueType());
    auto offset = lowerValue(index->index_expr.get());
    if (!array || mlir::failed(offset) ||
        mlir::failed(checkBounds(loc(index), *offset, array.getSize())))
      return mlir::failure();
    return smn::ElemOp::create(
               builder_, loc(index),
               placeOf(array.getElement()),
               *base, *offset)
        .getResult();
  }
  return mlir::failure();
}

// `&x`: a view of the place x names, for as long as it lives.
mlir::FailureOr<mlir::Value> MIRLowering::lowerAddrOf(AddrOfExprAST *addr) {
  if (!hasPlace(addr->operand.get()))
    return mlir::failure();
  auto place = lowerPlace(addr->operand.get());
  if (mlir::failed(place))
    return mlir::failure();
  auto holds = mlir::cast<smn::PlaceType>(place->getType()).getValueType();
  return smn::AddrOp::create(
             builder_, loc(addr),
             viewOf(holds),
             *place)
      .getResult();
}

// `x[i]` through an Indexer instance: its method is lent x's place, or a
// slot holding x's value when x names none.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerIndexerCall(IndexExprAST *index,
                              const sammine_util::QualifiedName &method) {
  auto location = loc(index);
  mlir::FailureOr<mlir::Value> place = mlir::failure();
  if (hasPlace(index->array_expr.get())) {
    place = lowerPlace(index->array_expr.get());
  } else {
    auto value = lowerValue(index->array_expr.get());
    if (mlir::failed(value))
      return mlir::failure();
    place = smn::LocalOp::create(
                builder_, location,
                placeOf(value->getType()),
                "")
                .getResult();
    smn::StoreOp::create(builder_, location, *value, *place);
  }
  if (mlir::failed(place))
    return mlir::failure();
  auto offset = lowerValue(index->index_expr.get());
  if (mlir::failed(offset))
    return mlir::failure();
  auto holds = mlir::cast<smn::PlaceType>(place->getType()).getValueType();
  auto view = smn::LendOp::create(
      builder_, location,
      viewOf(holds),
      *place);
  return callMethod(location, method, {view, *offset}, index->get_type());
}

/// Which field of its struct or tuple `access` reads.
std::optional<size_t>
MIRLowering::fieldIndex(FieldAccessExprAST *access) const {
  auto objectType = access->object_expr->get_type();
  if (objectType.type_kind == TypeKind::Tuple)
    return std::stoul(access->field_name);
  if (objectType.type_kind == TypeKind::Struct)
    return std::get<StructType>(objectType.type_data)
        .get_field_index(access->field_name);
  return std::nullopt;
}

// `p[i]`, for p a pointer, in an unsafe function: the type checker allows it
// nowhere else.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerPtrElemPlace(IndexExprAST *index) {
  if (index->array_expr->get_type().type_kind != TypeKind::Pointer)
    return mlir::failure();
  auto ptr = lowerValue(index->array_expr.get());
  if (mlir::failed(ptr))
    return mlir::failure();
  auto offset = lowerValue(index->index_expr.get());
  if (mlir::failed(offset))
    return mlir::failure();
  auto pointee = mlir::cast<smn::PtrType>(ptr->getType()).getPointee();
  return smn::PtrElemOp::create(
             builder_, loc(index),
             placeOf(pointee), *ptr, *offset)
      .getResult();
}

// Reading a place copies a value that owns nothing, and moves one that does
// out of it.
mlir::Value MIRLowering::read(mlir::Location location, mlir::Value place) {
  auto valueType = mlir::cast<smn::PlaceType>(place.getType()).getValueType();
  if (smn::ownsMemory(valueType))
    return smn::MoveOp::create(builder_, location, valueType, place);
  return smn::CopyOp::create(builder_, location, valueType, place);
}

// `x = v`, `*p = v`, `s.f = v` or `a[i] = v`: a store to the place on the
// left.
mlir::FailureOr<mlir::Value> MIRLowering::lowerAssign(BinaryExprAST *assign) {
  auto value = lowerValue(assign->RHS.get());
  if (mlir::failed(value))
    return mlir::failure();
  auto place = lowerPlace(assign->LHS.get());
  if (mlir::failed(place))
    return mlir::failure();
  smn::StoreOp::create(builder_, loc(assign), *value, *place);
  return mlir::Value();
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

mlir::FailureOr<mlir::Value>
MIRLowering::lowerNegation(UnaryNegExprAST *neg) {
  if (!neg->get_type().is_integer() && !neg->get_type().is_float())
    return mlir::failure();
  auto value = lowerValue(neg->operand.get());
  if (mlir::failed(value))
    return mlir::failure();
  return emitNegation(builder_, loc(neg), *value);
}

// An array's length is in its type.
mlir::FailureOr<mlir::Value> MIRLowering::lowerLen(LenExprAST *len) {
  auto operand = len->operand->get_type();
  if (operand.type_kind != TypeKind::Array)
    return mlir::failure();
  auto size = std::get<ArrayType>(operand.type_data).get_size();
  return intConstant(loc(len), builder_.getI32Type(),
                     static_cast<int64_t>(size));
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerBool(BoolExprAST *b) {
  return intConstant(loc(b),
                                            builder_.getI1Type(), b->b ? 1 : 0);
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerChar(CharExprAST *c) {
  return intConstant(loc(c),
                                            builder_.getI8Type(),
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

// An operator through the instance the type checker chose: one the program
// declares is a call to its method, one the compiler provides lowerOperator.
mlir::FailureOr<mlir::Value> MIRLowering::lowerBinary(BinaryExprAST *bin) {
  if (auto *bp = props_.binary(bin->id()); bp && bp->resolved_op_method) {
    auto lhs = lowerValue(bin->LHS.get());
    if (mlir::failed(lhs))
      return mlir::failure();
    auto rhs = lowerValue(bin->RHS.get());
    if (mlir::failed(rhs))
      return mlir::failure();
    return callMethod(loc(bin), *bp->resolved_op_method, {*lhs, *rhs},
                      bin->get_type());
  }
  auto lhs = lowerValue(bin->LHS.get());
  if (mlir::failed(lhs))
    return mlir::failure();
  auto rhs = lowerValue(bin->RHS.get());
  if (mlir::failed(rhs))
    return mlir::failure();
  // The operands' type decides what the operator does; a comparison's own
  // type is bool.
  return lowerOperator(loc(bin), bin->Op->tok_type, *lhs, *rhs,
                       bin->LHS->get_type());
}

// `lhs op rhs`, operands of `type`, through the instance the compiler
// provides. On integers, chars, bools, and an
// integer-backed enum's backing integer, and on floats, the operator itself;
// pointers compare their addresses, as signed integers; arrays and enums
// with payloads take == and != (lowerEquality).
mlir::FailureOr<mlir::Value>
MIRLowering::lowerOperator(mlir::Location location, TokenType op,
                           mlir::Value lhs, mlir::Value rhs, const Type &type) {
  bool comparison = isComparisonOp(op);
  mlir::Value result;
  if (type.type_kind == TypeKind::Pointer) {
    if (!comparison)
      return mlir::failure();
    auto address = [&](mlir::Value ptr) -> mlir::Value {
      return smn::AddressOp::create(builder_, location,
                                    builder_.getI64Type(), ptr);
    };
    result = emitScalarComparison(builder_, location, address(lhs),
                                  address(rhs), op, /*is_float=*/false,
                                  /*is_unsigned=*/false);
  } else if (auto integer = integerKind(type)) {
    result = comparison ? emitScalarComparison(builder_, location, lhs, rhs, op,
                                               /*is_float=*/false, *integer)
                        : emitIntArithOp(builder_, location, lhs, rhs, op,
                                         *integer);
  } else if (type.is_float()) {
    result = comparison ? emitScalarComparison(builder_, location, lhs, rhs, op,
                                               /*is_float=*/true,
                                               /*is_unsigned=*/false)
                        : emitFloatArithOp(builder_, location, lhs, rhs, op);
  } else if (op == TokEQUAL || op == TokNOTEqual) {
    auto equal = lowerEquality(location, lhs, rhs, type);
    if (mlir::failed(equal) || op == TokEQUAL)
      return equal;
    auto one = intConstant(location,
                                                  builder_.getI1Type(), 1);
    return mlir::arith::XOrIOp::create(builder_, location, *equal, one)
        .getResult();
  }
  if (!result)
    return mlir::failure();
  return result;
}

// Whether `type` is an integer to operators, and if so whether unsigned: an
// integer, a char (signed) or a bool, or an integer-backed enum, by its
// backing integer.
std::optional<bool> MIRLowering::integerKind(const Type &type) {
  if (type.is_integer())
    return type.is_unsigned();
  if (type.type_kind == TypeKind::Char || type.type_kind == TypeKind::Bool)
    return false;
  if (type.type_kind == TypeKind::Enum) {
    auto &et = std::get<EnumType>(type.type_data);
    if (et.is_integer_backed())
      return et.get_backing_type() == TypeKind::U32_t ||
             et.get_backing_type() == TypeKind::U64_t;
  }
  return std::nullopt;
}

// `lhs == rhs` of `type`, as an i1. Arrays compare element by element,
// stopping at the first that differs; enums with payloads compare their
// tags, then the payload of the variant they share.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerEquality(mlir::Location location, mlir::Value lhs,
                           mlir::Value rhs, const Type &type) {
  auto i1 = builder_.getI1Type();
  auto *region = builder_.getInsertionBlock()->getParent();

  if (type.type_kind == TypeKind::Array) {
    auto &array = std::get<ArrayType>(type.type_data);
    auto mirArray = mlir::dyn_cast<smn::ArrayType>(lhs.getType());
    if (!mirArray)
      return mlir::failure();
    auto i64 = builder_.getI64Type();
    auto *header = &region->emplaceBlock();
    header->addArgument(i64, location);
    auto *body = &region->emplaceBlock();
    auto *after = &region->emplaceBlock();
    after->addArgument(i1, location);
    mlir::cf::BranchOp::create(builder_, location, header,
                               mlir::ValueRange{intConstant(location, i64, 0)});
    builder_.setInsertionPointToStart(header);
    auto index = header->getArgument(0);
    auto more = mlir::arith::CmpIOp::create(
        builder_, location, mlir::arith::CmpIPredicate::slt, index,
        intConstant(location, i64, static_cast<int64_t>(array.get_size())));
    mlir::cf::CondBranchOp::create(builder_, location, more, body,
                                   mlir::ValueRange{}, after,
                                   mlir::ValueRange{intConstant(location, i1, 1)});
    builder_.setInsertionPointToStart(body);
    auto element = mirArray.getElement();
    auto a = smn::ElementOp::create(builder_, location, element, lhs, index);
    auto b = smn::ElementOp::create(builder_, location, element, rhs, index);
    auto same = lowerOperator(location, TokEQUAL, a, b, array.get_element());
    if (mlir::failed(same))
      return mlir::failure();
    auto next = mlir::arith::AddIOp::create(builder_, location, index,
                                            intConstant(location, i64, 1));
    mlir::cf::CondBranchOp::create(builder_, location, *same, header,
                                   mlir::ValueRange{next}, after,
                                   mlir::ValueRange{intConstant(location, i1, 0)});
    continueIn(after);
    return after->getArgument(0);
  }

  if (type.type_kind != TypeKind::Enum)
    return mlir::failure();
  auto et = std::get<EnumType>(type.type_data);
  auto enumType = mlir::dyn_cast<smn::EnumType>(lhs.getType());
  if (!enumType)
    return mlir::failure();
  auto i32 = builder_.getI32Type();
  auto lhsTag = smn::TagOp::create(builder_, location, i32, lhs);
  auto rhsTag = smn::TagOp::create(builder_, location, i32, rhs);
  auto sameTag = mlir::arith::CmpIOp::create(
      builder_, location, mlir::arith::CmpIPredicate::eq, lhsTag, rhsTag);
  auto *check = &region->emplaceBlock();
  auto *after = &region->emplaceBlock();
  after->addArgument(i1, location);
  mlir::cf::CondBranchOp::create(builder_, location, sameTag, check,
                                 mlir::ValueRange{}, after,
                                 mlir::ValueRange{intConstant(location, i1, 0)});
  // The same variant: with no payload, equal; else equal if its fields are.
  builder_.setInsertionPointToStart(check);
  auto equalTrue = intConstant(location, i1, 1);
  llvm::SmallVector<int32_t> tags;
  llvm::SmallVector<mlir::Block *> dests;
  for (auto [v, variant] : llvm::enumerate(et.get_variants()))
    if (!variant.payload_types.empty()) {
      tags.push_back(static_cast<int32_t>(v));
      dests.push_back(&region->emplaceBlock());
    }
  branchOn(location, lhsTag, tags, dests, after, mlir::ValueRange{equalTrue});
  for (auto [tag, dest] : llvm::zip(tags, dests)) {
    builder_.setInsertionPointToStart(dest);
    auto variant = static_cast<uint64_t>(tag);
    auto &fields = et.get_variant(variant).payload_types;
    auto mirFields = enumType.getPayload(variant);
    mlir::Value all = intConstant(location, i1, 1);
    int64_t offset = 0;
    for (auto [i, field] : llvm::enumerate(fields)) {
      auto at = static_cast<uint64_t>(layout::advancePayloadOffset(offset, field));
      auto a = smn::PayloadOp::create(builder_, location, mirFields[i], lhs,
                                      variant, i, at);
      auto b = smn::PayloadOp::create(builder_, location, mirFields[i], rhs,
                                      variant, i, at);
      auto same = lowerOperator(location, TokEQUAL, a, b, field);
      if (mlir::failed(same))
        return mlir::failure();
      all = mlir::arith::AndIOp::create(builder_, location, all, *same);
    }
    mlir::cf::BranchOp::create(builder_, location, after,
                               mlir::ValueRange{all});
  }
  continueIn(after);
  return after->getArgument(0);
}

} // namespace sammine_lang
