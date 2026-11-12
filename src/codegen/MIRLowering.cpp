#include "codegen/MIRLowering.h"
#include "codegen/MLIRGenBinaryOps.h"
#include "codegen/TypeLayout.h"
#include "dialect/SammineDialect.h"
#include "util/Utilities.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "llvm/ADT/StringSet.h"

//! \file MIRLowering.cpp
//! \brief The constructs MIR handles so far: functions over scalars (integers,
//! floats, bool, char), pointers, structs and tuples of them, returning one or
//! `()`; struct and tuple literals, fields, `let (a, b) = t`; enums with
//! payloads, their variants and `case` on them; their
//! literals, strings among them, parameters, `let`, arithmetic, calls to
//! sammine and C functions, lending an owned pointer to a ptr<T>, `alloc`,
//! `free`, `*p` and `p[i]` read and written, negation, comparisons, `if`, `&&` and `||`,
//! statements and return.

namespace sammine_lang {

using namespace AST;

std::optional<mlir::Type> MIRLowering::mirType(const Type &type) const {
  llvm::StringSet<> enclosing;
  return mirType(type, enclosing);
}

std::optional<mlir::Type>
MIRLowering::mirType(const Type &type, llvm::StringSet<> &enclosing) const {
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
    auto pointee =
        mirType(std::get<PointerType>(type.type_data).get_pointee(), enclosing);
    if (!pointee)
      return std::nullopt;
    return smn::PtrType::get(builder_.getContext(), *pointee,
                             type.linearity == Linearity::Linear
                                 ? smn::Ownership::Owned
                                 : smn::Ownership::View);
  }
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(type.type_data);
    auto name = st.get_name().mangled();
    // A struct that reaches itself, through a pointer, has no MIR type yet.
    if (!enclosing.insert(name).second)
      return std::nullopt;
    llvm::SmallVector<mlir::Type> fields;
    for (auto &field : st.get_field_types()) {
      auto fieldType = mirType(field, enclosing);
      if (!fieldType)
        return std::nullopt;
      fields.push_back(*fieldType);
    }
    enclosing.erase(name);
    return smn::StructType::get(builder_.getContext(), name, fields);
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(type.type_data);
    auto name = et.get_name().mangled();
    // An integer-backed enum isn't lowered yet, nor an enum that reaches
    // itself through a pointer.
    if (et.is_integer_backed() || !enclosing.insert(name).second)
      return std::nullopt;
    llvm::SmallVector<mlir::Type> variants;
    int64_t payloadSize = 0;
    for (auto &variant : et.get_variants()) {
      llvm::SmallVector<mlir::Type> fields;
      for (auto &field : variant.payload_types) {
        auto fieldType = mirType(field, enclosing);
        if (!fieldType)
          return std::nullopt;
        fields.push_back(*fieldType);
      }
      variants.push_back(smn::TupleType::get(builder_.getContext(), fields));
      payloadSize = std::max(payloadSize,
                             layout::variantPayloadSize(variant.payload_types));
    }
    enclosing.erase(name);
    return smn::EnumType::get(builder_.getContext(), name, payloadSize,
                              variants);
  }
  case TypeKind::Tuple: {
    llvm::SmallVector<mlir::Type> elements;
    for (auto &element : std::get<TupleType>(type.type_data).get_element_types()) {
      auto elementType = mirType(element, enclosing);
      if (!elementType)
        return std::nullopt;
      elements.push_back(*elementType);
    }
    return smn::TupleType::get(builder_.getContext(), elements);
  }
  default:
    return std::nullopt;
  }
}

std::optional<mlir::FunctionType>
MIRLowering::mirFunctionType(const FunctionType &fn) const {
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

  // The body ends by returning, its implicit return made explicit by
  // GeneralSemanticsVisitor, on every path.
  if (mlir::failed(lowerStatements(ast->Block.get())) || !terminated())
    return false;

  (*func)->setAttr(smn::kMirAttr, builder_.getUnitAttr());
  decl->getBlock()->getOperations().insert(mlir::Block::iterator(decl),
                                           func.release());
  decl.erase();
  return true;
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
  auto outer = names_;
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
  names_ = std::move(outer);
  return last;
}

// A `let` of a value MIR has a type for names that value: the right side is
// lowered first, so `let x = x + 1` reads the x it shadows. `let (a, b) = t`
// names each element of t.
mlir::LogicalResult MIRLowering::lowerVarDef(VarDefAST *def) {
  // A `let (a, b) = t` is typed (); t's type is checked as it is lowered.
  if (!def->is_tuple_destructure && !mirType(def->get_type()))
    return mlir::failure();
  auto value = lowerExpr(def->Expression.get());
  if (mlir::failed(value) || !*value)
    return mlir::failure();
  if (!def->is_tuple_destructure) {
    names_[def->TypedVar->name] = *value;
    return mlir::success();
  }
  auto tuple = mlir::dyn_cast<smn::TupleType>(value->getType());
  if (!tuple || tuple.getElements().size() != def->destructure_vars.size())
    return mlir::failure();
  for (auto [i, var] : llvm::enumerate(def->destructure_vars))
    names_[var->name] =
        smn::ExtractOp::create(builder_, loc(def), tuple.getElements()[i],
                               *value, i)
            .getResult();
  return mlir::success();
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
  if (auto *caseExpr = llvm::dyn_cast<CaseExprAST>(expr))
    return lowerCase(caseExpr);
  if (auto *neg = llvm::dyn_cast<UnaryNegExprAST>(expr)) {
    if (!neg->get_type().is_integer() && !neg->get_type().is_float())
      return mlir::failure();
    auto value = lowerExpr(neg->operand.get());
    if (mlir::failed(value) || !*value)
      return mlir::failure();
    return emitNegation(builder_, loc(neg), *value);
  }
  if (auto *call = llvm::dyn_cast<CallExprAST>(expr))
    return lowerCall(call);
  if (auto *alloc = llvm::dyn_cast<AllocExprAST>(expr))
    return lowerAlloc(alloc);
  if (auto *free = llvm::dyn_cast<FreeExprAST>(expr))
    return lowerFree(free);
  if (llvm::isa<DerefExprAST>(expr))
    return lowerPlaceRead(expr);
  if (auto *index = llvm::dyn_cast<IndexExprAST>(expr)) {
    if (index->array_expr->get_type().type_kind != TypeKind::Pointer)
      return mlir::failure();
    return lowerPlaceRead(expr);
  }
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
// instance's method or a C function, whether its body goes through MIR or not.
mlir::FailureOr<mlir::Value> MIRLowering::lowerCall(CallExprAST *call) {
  // Not lowered yet: enum constructors, instances the compiler provides, and
  // partial application.
  auto *cp = props_.call(call->id());
  if (cp->is_enum_constructor) {
    llvm::SmallVector<ExprAST *> payload;
    for (auto &arg : call->arguments)
      payload.push_back(arg.get());
    return lowerVariant(loc(call), call->get_type(), cp->enum_variant_index,
                        payload);
  }
  if (cp->builtin_instance || cp->is_partial)
    return mlir::failure();
  auto name =
      props_.symbol(cp->resolved_name ? *cp->resolved_name : call->functionName);
  // Nor a call through a function value, which names no function.
  auto callee = module_.lookupSymbol<mlir::FunctionOpInterface>(name);
  if (!callee)
    return mlir::failure();
  auto type =
      mirFunctionType(std::get<FunctionType>(cp->callee_func_type->type_data));
  if (!type)
    return mlir::failure();
  // Nor a struct or tuple passed to or from C, which C moves in registers or
  // memory as cabi decides, in emitCCall.
  auto isAggregate = [](mlir::Type t) {
    return mlir::isa<smn::StructType, smn::TupleType>(t);
  };
  if (mlir::isa<mlir::LLVM::LLVMFuncOp>(callee.getOperation()) &&
      (llvm::any_of(type->getInputs(), isAggregate) ||
       llvm::any_of(type->getResults(), isAggregate)))
    return mlir::failure();

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

// An argument for a parameter of type `param`. An owned pointer passed to a
// ptr<T> is lent: the callee gets a view of what it points at, and the caller
// keeps it.
mlir::FailureOr<mlir::Value> MIRLowering::lowerArgument(ExprAST *arg,
                                                        mlir::Type param) {
  auto value = lowerExpr(arg);
  if (mlir::failed(value) || !*value)
    return mlir::failure();
  if (value->getType() == param)
    return *value;
  auto ptr = mlir::dyn_cast<smn::PtrType>(value->getType());
  if (!ptr || !ptr.isOwned() ||
      param != smn::PtrType::get(builder_.getContext(), ptr.getPointee(),
                                 smn::Ownership::View))
    return mlir::failure();
  return lend(*value, loc(arg));
}

// An argument passed through a C function's `...`. Nothing declares its
// type, and C only reads what sammine passes it (the type checker keeps
// addresses out), so an owned pointer is lent, as the linear checker has it.
mlir::FailureOr<mlir::Value> MIRLowering::lowerVariadicArgument(ExprAST *arg) {
  auto value = lowerExpr(arg);
  if (mlir::failed(value) || !*value)
    return mlir::failure();
  if (smn::ownsMemory(value->getType()))
    return lend(*value, loc(arg));
  return *value;
}

mlir::Value MIRLowering::lend(mlir::Value owner, mlir::Location loc) {
  auto pointee = mlir::cast<smn::PtrType>(owner.getType()).getPointee();
  auto *ctx = builder_.getContext();
  auto place = smn::DerefOp::create(builder_, loc,
                                    smn::PlaceType::get(ctx, pointee), owner);
  return smn::LendOp::create(
             builder_, loc,
             smn::PtrType::get(ctx, pointee, smn::Ownership::View), place)
      .getResult();
}

// The type an if's or a case's value has in MIR: none if it is () or never
// reached; failure if MIR has no type for it.
mlir::FailureOr<std::optional<mlir::Type>>
MIRLowering::branchResultType(const Type &type) const {
  if (type.type_kind == TypeKind::Unit || type.type_kind == TypeKind::Never)
    return std::optional<mlir::Type>();
  auto mir = mirType(type);
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
    auto outer = names_;
    if (arm.bind)
      arm.bind();
    auto value = lowerStatements(arm.body);
    names_ = std::move(outer);
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
  after->moveBefore(region, region->end());
  builder_.setInsertionPointToStart(after);
  return resultType ? after->getArgument(0) : mlir::Value();
}

// `if c { a } else { b }`: c branches to a block for each side.
mlir::FailureOr<mlir::Value> MIRLowering::lowerIf(IfExprAST *ifExpr) {
  auto resultType = branchResultType(ifExpr->get_type());
  if (mlir::failed(resultType))
    return mlir::failure();
  auto cond = lowerExpr(ifExpr->bool_expr.get());
  if (mlir::failed(cond) || !*cond)
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
  if (scrutineeType.type_kind != TypeKind::Enum || caseExpr->arms.empty())
    return mlir::failure();
  auto et = std::get<EnumType>(scrutineeType.type_data);
  auto resultType = branchResultType(caseExpr->get_type());
  if (mlir::failed(resultType))
    return mlir::failure();
  auto value = lowerExpr(caseExpr->scrutinee.get());
  if (mlir::failed(value) || !*value)
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
        names_[name] = smn::PayloadOp::create(builder_, location,
                                              mirPayload[i], scrutinee,
                                              pattern.variant_index, i,
                                              static_cast<uint64_t>(at))
                           .getResult();
      }
    }});
  }
  if (!otherwise) {
    otherwise = dests.pop_back_val();
    tags.pop_back();
  }
  llvm::SmallVector<mlir::ValueRange> noOperands(dests.size());
  mlir::cf::SwitchOp::create(builder_, location, tag, otherwise,
                             mlir::ValueRange{}, tags, dests, noOperands);
  return lowerArms(location, *resultType, arms);
}

// `Option<i64>::Some(x)`, or a variant with no payload, `None`: its payload
// fields at the offsets layout:: gives them, as the AST codegen puts them.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerVariant(mlir::Location location, const Type &type,
                          size_t index, llvm::ArrayRef<ExprAST *> payload) {
  auto mir = mirType(type);
  if (!mir)
    return mlir::failure();
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
    auto value = lowerExpr(expr);
    if (mlir::failed(value) || !*value || value->getType() != mirField)
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
  auto lhs = lowerExpr(bin->LHS.get());
  if (mlir::failed(lhs) || !*lhs)
    return mlir::failure();
  bool isAnd = bin->Op->tok_type == TokAND;
  auto location = loc(bin);
  auto shortCut = mlir::arith::ConstantIntOp::create(
      builder_, location, builder_.getI1Type(), isAnd ? 0 : 1);

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
  auto rhs = lowerExpr(bin->RHS.get());
  if (mlir::failed(rhs) || !*rhs || terminated())
    return mlir::failure();
  mlir::cf::BranchOp::create(builder_, location, after,
                             mlir::ValueRange{*rhs});
  after->moveBefore(region, region->end());
  builder_.setInsertionPointToStart(after);
  return after->getArgument(0);
}

// The values of `exprs`, in order, as the fields of an aggregate of `type`.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerAggregate(mlir::Location location, mlir::Type type,
                            llvm::ArrayRef<mlir::Type> fieldTypes,
                            llvm::ArrayRef<std::pair<size_t, ExprAST *>> exprs) {
  llvm::SmallVector<mlir::Value> fields(fieldTypes.size());
  for (auto [index, expr] : exprs) {
    auto value = lowerExpr(expr);
    if (mlir::failed(value) || !*value || value->getType() != fieldTypes[index])
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
  auto type = mirType(lit->get_type());
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
  auto type = mirType(tuple->get_type());
  if (!type)
    return mlir::failure();
  llvm::SmallVector<std::pair<size_t, ExprAST *>> exprs;
  for (auto [i, element] : llvm::enumerate(tuple->elements))
    exprs.push_back({i, element.get()});
  return lowerAggregate(loc(tuple), *type,
                        mlir::cast<smn::TupleType>(*type).getElements(), exprs);
}

// `p.x`, or `t.0` of a tuple.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerFieldAccess(FieldAccessExprAST *access) {
  auto objectType = access->object_expr->get_type();
  std::optional<size_t> index;
  if (objectType.type_kind == TypeKind::Tuple)
    index = std::stoul(access->field_name);
  else if (objectType.type_kind == TypeKind::Struct)
    index = std::get<StructType>(objectType.type_data)
                .get_field_index(access->field_name);
  auto type = mirType(access->get_type());
  if (!index || !type)
    return mlir::failure();
  auto object = lowerExpr(access->object_expr.get());
  if (mlir::failed(object) || !*object)
    return mlir::failure();
  return smn::ExtractOp::create(builder_, loc(access), *type, *object, *index)
      .getResult();
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

// Reading `*p` or `p[i]`, for p a pointer.
mlir::FailureOr<mlir::Value> MIRLowering::lowerPlaceRead(ExprAST *expr) {
  auto place = lowerPlace(expr);
  if (mlir::failed(place))
    return mlir::failure();
  return read(loc(expr), *place);
}

// The place `target` names, if MIR has one: `*p`, or `p[i]` of a pointer.
mlir::FailureOr<mlir::Value> MIRLowering::lowerPlace(ExprAST *target) {
  if (auto *deref = llvm::dyn_cast<DerefExprAST>(target))
    return lowerDerefPlace(deref);
  if (auto *index = llvm::dyn_cast<IndexExprAST>(target))
    return lowerPtrElemPlace(index);
  return mlir::failure();
}

// `p[i]`, for p a pointer, in an unsafe function: the type checker allows it
// nowhere else.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerPtrElemPlace(IndexExprAST *index) {
  if (index->array_expr->get_type().type_kind != TypeKind::Pointer)
    return mlir::failure();
  auto ptr = lowerExpr(index->array_expr.get());
  if (mlir::failed(ptr) || !*ptr)
    return mlir::failure();
  auto offset = lowerExpr(index->index_expr.get());
  if (mlir::failed(offset) || !*offset)
    return mlir::failure();
  auto pointee = mlir::cast<smn::PtrType>(ptr->getType()).getPointee();
  return smn::PtrElemOp::create(
             builder_, loc(index),
             smn::PlaceType::get(builder_.getContext(), pointee), *ptr, *offset)
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

// `*p = v` or `p[i] = v`. Writing a field isn't lowered yet.
mlir::FailureOr<mlir::Value> MIRLowering::lowerAssign(BinaryExprAST *assign) {
  auto value = lowerExpr(assign->RHS.get());
  if (mlir::failed(value) || !*value)
    return mlir::failure();
  auto place = lowerPlace(assign->LHS.get());
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

mlir::FailureOr<mlir::Value> MIRLowering::lowerString(StringExprAST *str) {
  auto type = mirType(str->get_type());
  if (!type)
    return mlir::failure();
  return smn::StringOp::create(builder_, loc(str), *type, str->string_content)
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
  if (auto *vp = props_.variable(var->id()); vp && vp->is_enum_unit_variant)
    return lowerVariant(loc(var), var->get_type(), vp->enum_variant_index, {});
  auto it = names_.find(var->variableName.mangled());
  if (it == names_.end())
    return mlir::failure();
  return it->second;
}

// Integer or float arithmetic, or a comparison of integers, bools or floats,
// through a built-in instance; an instance the program declares is a call,
// which MIR doesn't lower yet.
mlir::FailureOr<mlir::Value> MIRLowering::lowerBinary(BinaryExprAST *bin) {
  auto *bp = props_.binary(bin->id());
  if (bp && bp->resolved_op_method)
    return mlir::failure();
  auto op = bin->Op->tok_type;
  bool arithmetic = op == TokADD || op == TokSUB || op == TokMUL ||
                    op == TokDIV || op == TokMOD;
  // A comparison's operands decide how it compares; its own type is bool.
  auto type = bin->Op->is_comparison() ? bin->LHS->get_type() : bin->get_type();
  bool scalar = type.is_integer() || type.is_float() ||
                (bin->Op->is_comparison() && type.type_kind == TypeKind::Bool);
  if (!(arithmetic || bin->Op->is_comparison()) || !scalar)
    return mlir::failure();
  auto lhs = lowerExpr(bin->LHS.get());
  if (mlir::failed(lhs) || !*lhs)
    return mlir::failure();
  auto rhs = lowerExpr(bin->RHS.get());
  if (mlir::failed(rhs) || !*rhs)
    return mlir::failure();
  if (bin->Op->is_comparison())
    return emitScalarComparison(builder_, loc(bin), *lhs, *rhs, op,
                                type.is_float(), type.is_unsigned());
  if (type.is_float())
    return emitFloatArithOp(builder_, loc(bin), *lhs, *rhs, op);
  return emitIntArithOp(builder_, loc(bin), *lhs, *rhs, op,
                        type.is_unsigned());
}

} // namespace sammine_lang
