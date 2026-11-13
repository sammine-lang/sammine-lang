#include "codegen/MIRLowering.h"
#include "codegen/ScalarOps.h"
#include "typecheck/BuiltinInstances.h"

//! \file MIRLoweringCalls.cpp
//! \brief Calls to sammine and C functions, methods and function values,
//! partial application, and the arguments they take: an owned pointer lent to a
//! `ptr<T>` parameter.

namespace sammine_lang {

using namespace AST;

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
  auto name = props_.symbol(cp->resolved_name ? *cp->resolved_name
                                              : call->functionName);
  // A name no function has is a binding holding a function value.
  auto callee = module_.lookupSymbol<mlir::FunctionOpInterface>(name);
  if (!callee)
    return lowerIndirectCall(call);
  auto type = types_.mirFunctionType(
      std::get<FunctionType>(cp->callee_func_type->type_data));
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
  auto op =
      smn::CallOp::create(builder_, loc(call), type->getResults(), name, args);
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
    auto value = lowerArgument(arg.get(), param);
    if (mlir::failed(value))
      return mlir::failure();
    bound.push_back(*value);
  }
  return smn::ClosureOp::create(builder_, loc(call), *result, name, bound)
      .getResult();
}

// An argument for a parameter of type `param`. An owned pointer passed to a
// ptr<T> is lent: the callee, or the closure a partial application makes,
// gets a view of what it points at, and the caller keeps it.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerArgument(ExprAST *arg, mlir::Type param) {
  auto type = types_.mirType(arg->get_type());
  auto ptr = type ? mlir::dyn_cast<smn::PtrType>(*type) : smn::PtrType();
  if (!ptr || !ptr.isOwned() || param != viewOf(ptr.getPointee())) {
    auto value = lowerValue(arg);
    if (mlir::failed(value) || value->getType() != param)
      return mlir::failure();
    return *value;
  }
  auto place = lowerPointee(arg, loc(arg));
  if (mlir::failed(place))
    return mlir::failure();
  return lend(*place, loc(arg));
}

// An argument passed through a C function's `...`. Nothing declares its
// type, and C only reads what sammine passes it (the type checker keeps
// addresses out), so an owned pointer is lent, as the linear checker has it.
mlir::FailureOr<mlir::Value> MIRLowering::lowerVariadicArgument(ExprAST *arg) {
  auto type = types_.mirType(arg->get_type());
  if (type && smn::ownsMemory(*type)) {
    auto place = lowerPointee(arg, loc(arg));
    if (mlir::failed(place))
      return mlir::failure();
    return lend(*place, loc(arg));
  }
  return lowerValue(arg);
}

mlir::Value MIRLowering::lend(mlir::Value place, mlir::Location loc) {
  auto type =
      viewOf(mlir::cast<smn::PlaceType>(place.getType()).getValueType());
  return smn::LendOp::create(builder_, loc, type, place).getResult();
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

// A call to `method`, a method of an instance the program declares, chosen
// by the type checker: an operator, or `as` through Convert. Its arguments
// are the operands, by value.
mlir::FailureOr<mlir::Value> MIRLowering::callMethod(
    mlir::Location location, const sammine_util::QualifiedName &method,
    mlir::ValueRange args, const Type &result, llvm::StringRef written) {
  auto type = types_.mirType(result);
  auto name = props_.symbol(method);
  if (!type || !module_.lookupSymbol<mlir::func::FuncOp>(name))
    return mlir::failure();
  return smn::CallOp::create(builder_, location, mlir::TypeRange{*type}, name,
                             args, written)
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
    place =
        smn::LocalOp::create(builder_, location, placeOf(value->getType()), "")
            .getResult();
    smn::StoreOp::create(builder_, location, *value, *place);
  }
  if (mlir::failed(place))
    return mlir::failure();
  auto offset = lowerValue(index->index_expr.get());
  if (mlir::failed(offset))
    return mlir::failure();
  auto holds = mlir::cast<smn::PlaceType>(place->getType()).getValueType();
  auto view = smn::LendOp::create(builder_, location, viewOf(holds), *place);
  return callMethod(location, method, {view, *offset}, index->get_type(), "[]");
}

} // namespace sammine_lang
