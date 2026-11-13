#include "codegen/MIRLowering.h"

//! \file MIRLoweringPlaces.cpp
//! \brief Places and what reads and writes them: bindings whose address is
//! taken,
//! `*p`, `p[i]`, fields and elements; `alloc`, `drop`, `&` and `=`.

namespace sammine_lang {

using namespace AST;

// alloc<T>(n): an owned pointer to room for n values of T, none stored yet.
mlir::FailureOr<mlir::Value> MIRLowering::lowerAlloc(AllocExprAST *alloc) {
  auto type = types_.mirType(alloc->get_type());
  auto count = lowerValue(alloc->operand.get());
  if (!type || mlir::failed(count))
    return mlir::failure();
  return smn::AllocOp::create(builder_, loc(alloc), *type, *count).getResult();
}

mlir::FailureOr<mlir::Value> MIRLowering::lowerDrop(DropExprAST *drop) {
  auto value = lowerValue(drop->operand.get());
  if (mlir::failed(value))
    return mlir::failure();
  smn::DropOp::create(builder_, loc(drop), *value);
  return mlir::Value();
}

// The place `*p` is, for `p` a pointer MIR has a type for.
mlir::FailureOr<mlir::Value> MIRLowering::lowerDerefPlace(DerefExprAST *deref) {
  return lowerPointee(deref->operand.get(), loc(deref));
}

mlir::FailureOr<mlir::Value>
MIRLowering::lowerPointee(ExprAST *pointer, mlir::Location location) {
  if (!types_.mirType(pointer->get_type()))
    return mlir::failure();
  // A pointer held in a place stays there: reaching what it points at only
  // reads it, as `**pp` reads `*pp`.
  if (hasPlace(pointer)) {
    auto place = lowerPlace(pointer);
    if (mlir::failed(place))
      return mlir::failure();
    return smn::ThroughOp::create(builder_, location, *place).getResult();
  }
  auto ptr = lowerValue(pointer);
  if (mlir::failed(ptr))
    return mlir::failure();
  auto pointee = mlir::cast<smn::PtrType>(ptr->getType()).getPointee();
  return smn::DerefOp::create(builder_, location, placeOf(pointee), *ptr)
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
    return smn::FieldOp::create(builder_, loc(field), placeOf(*type), *base,
                                *index)
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
    return smn::ElemOp::create(builder_, loc(index),
                               placeOf(array.getElement()), *base, *offset)
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
  return smn::AddrOp::create(builder_, loc(addr), viewOf(holds), *place)
      .getResult();
}

// `p[i]`, for p a pointer, in an unsafe function: the type checker allows it
// nowhere else.
mlir::FailureOr<mlir::Value>
MIRLowering::lowerPtrElemPlace(IndexExprAST *index) {
  if (index->array_expr->get_type().type_kind != TypeKind::Pointer)
    return mlir::failure();
  auto base = lowerPointee(index->array_expr.get(), loc(index));
  if (mlir::failed(base))
    return mlir::failure();
  auto offset = lowerValue(index->index_expr.get());
  if (mlir::failed(offset))
    return mlir::failure();
  return smn::PtrElemOp::create(builder_, loc(index), base->getType(), *base,
                                *offset)
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

} // namespace sammine_lang
