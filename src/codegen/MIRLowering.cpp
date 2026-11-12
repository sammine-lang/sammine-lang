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
#include "llvm/ADT/StringSet.h"

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

// A function returning an array returns it through a pointer its caller
// passes (sret), as the AST codegen and its callers agree; MIR doesn't yet.
bool returnsArray(mlir::FunctionType type) {
  return llvm::any_of(type.getResults(), llvm::IsaPred<smn::ArrayType>);
}

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
    // An integer-backed enum is its backing integer, as in the AST codegen.
    if (et.is_integer_backed()) {
      auto backing = et.get_backing_type();
      return builder_.getIntegerType(
          backing == TypeKind::I64_t || backing == TypeKind::U64_t ? 64 : 32);
    }
    // An enum that reaches itself through a pointer has no MIR type yet.
    if (!enclosing.insert(name).second)
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
  case TypeKind::Function: {
    // A function value: a C function's `...` has no MIR type, and a
    // function returning an array returns it through a pointer (sret).
    auto &fn = std::get<FunctionType>(type.type_data);
    auto signature = mirFunctionType(fn);
    if (fn.is_var_arg() || !signature || returnsArray(*signature))
      return std::nullopt;
    return smn::FnType::get(builder_.getContext(), *signature);
  }
  case TypeKind::Array: {
    auto &array = std::get<ArrayType>(type.type_data);
    auto element = mirType(array.get_element(), enclosing);
    if (!element)
      return std::nullopt;
    return smn::ArrayType::get(builder_.getContext(),
                               static_cast<int64_t>(array.get_size()),
                               *element);
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
  auto signature = mirFunctionType(std::get<FunctionType>(type.type_data));
  if (!signature || returnsArray(*signature))
    return std::nullopt;
  return signature;
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
  // returns (), or its last value, as the AST codegen has it.
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
      smn::PlaceType::get(builder_.getContext(), value.getType()), name);
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
  if (auto *len = llvm::dyn_cast<LenExprAST>(expr)) {
    auto operand = len->operand->get_type();
    if (operand.type_kind != TypeKind::Array)
      return mlir::failure();
    return mlir::arith::ConstantIntOp::create(
               builder_, loc(len), builder_.getI32Type(),
               static_cast<int64_t>(
                   std::get<ArrayType>(operand.type_data).get_size()))
        .getResult();
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
      auto value = lowerExpr(arg.get());
      if (mlir::failed(value) || !*value)
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
      mirFunctionType(std::get<FunctionType>(cp->callee_func_type->type_data));
  if (!type)
    return mlir::failure();
  if (!callableFromMIR(callee, *type))
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

// Whether MIR can call `callee`, of MIR signature `type`, yet. Not a struct,
// tuple or array passed to or from C, which C moves in registers or memory
// as cabi decides, in emitCCall; nor an array passed to a kernel's wrapper,
// which takes it by pointer, or returned (sret).
bool MIRLowering::callableFromMIR(mlir::FunctionOpInterface callee,
                                  mlir::FunctionType type) const {
  auto isAggregate = [](mlir::Type t) {
    return mlir::isa<smn::StructType, smn::TupleType, smn::ArrayType>(t);
  };
  bool passesAggregate = llvm::any_of(type.getInputs(), isAggregate) ||
                         llvm::any_of(type.getResults(), isAggregate);
  bool passesArray =
      llvm::any_of(type.getInputs(), llvm::IsaPred<smn::ArrayType>);
  return !(mlir::isa<mlir::LLVM::LLVMFuncOp>(callee.getOperation()) &&
           passesAggregate) &&
         !(callee->hasAttr("sammine.kernel_wrapper") && passesArray) &&
         !returnsArray(type);
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
  auto result = mirType(call->get_type());
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
  return lend(*value, loc(arg), view);
}

// An argument passed through a C function's `...`. Nothing declares its
// type, and C only reads what sammine passes it (the type checker keeps
// addresses out), so an owned pointer is lent, as the linear checker has it.
mlir::FailureOr<mlir::Value> MIRLowering::lowerVariadicArgument(ExprAST *arg) {
  auto value = lowerExpr(arg);
  if (mlir::failed(value) || !*value)
    return mlir::failure();
  if (smn::ownsMemory(value->getType()))
    return lend(*value, loc(arg), ViewLasts::Call);
  return *value;
}

mlir::Value MIRLowering::lend(mlir::Value owner, mlir::Location loc,
                              ViewLasts view) {
  auto pointee = mlir::cast<smn::PtrType>(owner.getType()).getPointee();
  auto *ctx = builder_.getContext();
  auto place = smn::DerefOp::create(builder_, loc,
                                    smn::PlaceType::get(ctx, pointee), owner);
  auto type = smn::PtrType::get(ctx, pointee, smn::Ownership::View);
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
  if (caseExpr->arms.empty())
    return mlir::failure();
  if (scrutineeType.type_kind != TypeKind::Enum ||
      std::get<EnumType>(scrutineeType.type_data).is_integer_backed())
    return lowerScalarCase(caseExpr);
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

// `for i in a..b { body }`, as the AST codegen does it: Range { a, b } goes
// round a loop, each turn calling Iterator<Range<T>, T>::next on it. Some
// gives the next range and i, for the body, then the loop goes round with
// that range; None leaves the loop.
mlir::FailureOr<mlir::Value> MIRLowering::lowerFor(ForExprAST *forExpr) {
  auto *forProps = props_.for_loop(forExpr->id());
  if (!forProps || !forProps->range_type)
    return mlir::failure();
  auto elementType = forExpr->start->get_type();
  auto iteration = rangeIteration(*forProps->range_type, elementType);
  auto rangeType = mirType(*forProps->range_type);
  auto optionType = mirType(iteration.option);
  auto stepType = mirType(iteration.step);
  auto element = mirType(elementType);
  if (!rangeType || !optionType || !stepType || !element ||
      !module_.lookupSymbol<mlir::FunctionOpInterface>(iteration.next))
    return mlir::failure();
  auto location = loc(forExpr);

  auto start = lowerExpr(forExpr->start.get());
  if (mlir::failed(start) || !*start)
    return mlir::failure();
  auto end = lowerExpr(forExpr->end.get());
  if (mlir::failed(end) || !*end)
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
  auto some = mlir::arith::ConstantIntOp::create(builder_, location,
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
  auto outer = names_;
  bind(forExpr->loop_var,
       smn::ExtractOp::create(builder_, location, *element, step, 1),
       location);
  auto value = lowerStatements(forExpr->body.get());
  names_ = std::move(outer);
  if (mlir::failed(value))
    return mlir::failure();
  if (!terminated())
    mlir::cf::BranchOp::create(builder_, location, header,
                               mlir::ValueRange{next});

  exit->moveBefore(region, region->end());
  builder_.setInsertionPointToStart(exit);
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
  auto value = lowerExpr(caseExpr->scrutinee.get());
  if (mlir::failed(value) || !*value)
    return mlir::failure();
  auto flagType = mlir::dyn_cast<mlir::IntegerType>(value->getType());
  if (!flagType)
    return mlir::failure();

  // The value each arm matches, as the AST codegen reads its pattern.
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
// fields at the offsets layout:: gives them, as the AST codegen puts them.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerVariant(mlir::Location location, const Type &type,
                          size_t index, llvm::ArrayRef<ExprAST *> payload) {
  auto mir = mirType(type);
  if (!mir)
    return mlir::failure();
  // A variant of an integer-backed enum is its discriminant.
  if (auto backing = mlir::dyn_cast<mlir::IntegerType>(*mir)) {
    auto discriminant =
        std::get<EnumType>(type.type_data).get_variant(index).discriminant_value;
    if (!discriminant || !payload.empty())
      return mlir::failure();
    return mlir::arith::ConstantIntOp::create(builder_, location, backing,
                                              *discriminant)
        .getResult();
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

// A function named as a value, `apply(f, x)`: f binding nothing.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerFunctionValue(VariableExprAST *var) {
  auto type = mirType(var->get_type());
  auto name = props_.symbol(var->variableName);
  auto callee = module_.lookupSymbol<mlir::FunctionOpInterface>(name);
  if (!type || !mlir::isa<smn::FnType>(*type) || !callee ||
      !callableFromMIR(callee,
                       mlir::cast<smn::FnType>(*type).getSignature()))
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
  auto type = mirType(access->get_type());
  if (!index || !type)
    return mlir::failure();
  auto object = lowerExpr(access->object_expr.get());
  if (mlir::failed(object) || !*object)
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
  auto type = mirType(result);
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
    auto to = mirType(key.type_args[1]);
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
  auto type = mirType(array->get_type());
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

// `a[i]` of an array: i is checked first, as the AST codegen checks it, and
// one out of bounds reported on stderr before the program exits.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerArrayIndex(IndexExprAST *index) {
  auto array = lowerExpr(index->array_expr.get());
  if (mlir::failed(array) || !*array)
    return mlir::failure();
  auto offset = lowerExpr(index->index_expr.get());
  if (mlir::failed(offset) || !*offset)
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
// size go to stderr through dprintf, then exit(1), as in the AST codegen;
// in it, lowering goes on in a block of its own.
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
  auto sizeValue = mlir::arith::ConstantIntOp::create(builder_, location, i64,
                                                      size);
  auto zero = mlir::arith::ConstantIntOp::create(builder_, location, i64, 0);
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
      smn::PtrType::get(builder_.getContext(), builder_.getI8Type(),
                        smn::Ownership::View),
      "index %lld out of bounds for an array of length %lld\n");
  auto stderrFd =
      mlir::arith::ConstantIntOp::create(builder_, location, i32, 2);
  smn::CallOp::create(builder_, location, mlir::TypeRange{i32}, kDprintfFunc,
                      mlir::ValueRange{stderrFd, message, wide, sizeValue});
  auto one = mlir::arith::ConstantIntOp::create(builder_, location, i32, 1);
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
  auto value = lowerExpr(cast->operand.get());
  if (mlir::failed(value) || !*value)
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

// Whether `expr` names a place, as the AST codegen's has_address has it: a
// binding in a slot, what a pointer points at, or a field or array element
// of a place, or an element past a pointer. Lowers nothing.
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
    auto type = mirType(field->get_type());
    if (!index || !type)
      return mlir::failure();
    return smn::FieldOp::create(
               builder_, loc(field),
               smn::PlaceType::get(builder_.getContext(), *type), *base, *index)
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
    auto offset = lowerExpr(index->index_expr.get());
    if (!array || mlir::failed(offset) || !*offset ||
        mlir::failed(checkBounds(loc(index), *offset, array.getSize())))
      return mlir::failure();
    return smn::ElemOp::create(
               builder_, loc(index),
               smn::PlaceType::get(builder_.getContext(), array.getElement()),
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
             smn::PtrType::get(builder_.getContext(), holds,
                               smn::Ownership::View),
             *place)
      .getResult();
}

// `x[i]` through an Indexer instance: its method is lent x's place, or a
// slot holding x's value when x names none, as the AST codegen does.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerIndexerCall(IndexExprAST *index,
                              const sammine_util::QualifiedName &method) {
  auto location = loc(index);
  mlir::FailureOr<mlir::Value> place = mlir::failure();
  if (hasPlace(index->array_expr.get())) {
    place = lowerPlace(index->array_expr.get());
  } else {
    auto value = lowerExpr(index->array_expr.get());
    if (mlir::failed(value) || !*value)
      return mlir::failure();
    place = smn::LocalOp::create(
                builder_, location,
                smn::PlaceType::get(builder_.getContext(), value->getType()),
                "")
                .getResult();
    smn::StoreOp::create(builder_, location, *value, *place);
  }
  if (mlir::failed(place))
    return mlir::failure();
  auto offset = lowerExpr(index->index_expr.get());
  if (mlir::failed(offset) || !*offset)
    return mlir::failure();
  auto holds = mlir::cast<smn::PlaceType>(place->getType()).getValueType();
  auto view = smn::LendOp::create(
      builder_, location,
      smn::PtrType::get(builder_.getContext(), holds, smn::Ownership::View),
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
    auto lhs = lowerExpr(bin->LHS.get());
    if (mlir::failed(lhs) || !*lhs)
      return mlir::failure();
    auto rhs = lowerExpr(bin->RHS.get());
    if (mlir::failed(rhs) || !*rhs)
      return mlir::failure();
    return callMethod(loc(bin), *bp->resolved_op_method, {*lhs, *rhs},
                      bin->get_type());
  }
  auto lhs = lowerExpr(bin->LHS.get());
  if (mlir::failed(lhs) || !*lhs)
    return mlir::failure();
  auto rhs = lowerExpr(bin->RHS.get());
  if (mlir::failed(rhs) || !*rhs)
    return mlir::failure();
  // The operands' type decides what the operator does; a comparison's own
  // type is bool.
  return lowerOperator(loc(bin), bin->Op->tok_type, *lhs, *rhs,
                       bin->LHS->get_type());
}

// `lhs op rhs`, operands of `type`, through the instance the compiler
// provides, as the AST codegen does it. On integers, chars, bools, and an
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
    auto one = mlir::arith::ConstantIntOp::create(builder_, location,
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
  auto constant = [&](mlir::Type t, int64_t v) -> mlir::Value {
    return mlir::arith::ConstantIntOp::create(builder_, location, t, v);
  };
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
                               mlir::ValueRange{constant(i64, 0)});
    builder_.setInsertionPointToStart(header);
    auto index = header->getArgument(0);
    auto more = mlir::arith::CmpIOp::create(
        builder_, location, mlir::arith::CmpIPredicate::slt, index,
        constant(i64, static_cast<int64_t>(array.get_size())));
    mlir::cf::CondBranchOp::create(builder_, location, more, body,
                                   mlir::ValueRange{}, after,
                                   mlir::ValueRange{constant(i1, 1)});
    builder_.setInsertionPointToStart(body);
    auto element = mirArray.getElement();
    auto a = smn::ElementOp::create(builder_, location, element, lhs, index);
    auto b = smn::ElementOp::create(builder_, location, element, rhs, index);
    auto same = lowerOperator(location, TokEQUAL, a, b, array.get_element());
    if (mlir::failed(same))
      return mlir::failure();
    auto next = mlir::arith::AddIOp::create(builder_, location, index,
                                            constant(i64, 1));
    mlir::cf::CondBranchOp::create(builder_, location, *same, header,
                                   mlir::ValueRange{next}, after,
                                   mlir::ValueRange{constant(i1, 0)});
    after->moveBefore(region, region->end());
    builder_.setInsertionPointToStart(after);
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
                                 mlir::ValueRange{constant(i1, 0)});
  // The same variant: with no payload, equal; else equal if its fields are.
  builder_.setInsertionPointToStart(check);
  auto equalTrue = constant(i1, 1);
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
    mlir::Value all = constant(i1, 1);
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
  after->moveBefore(region, region->end());
  builder_.setInsertionPointToStart(after);
  return after->getArgument(0);
}

} // namespace sammine_lang
