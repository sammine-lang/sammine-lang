#include "codegen/MIRTypes.h"
#include "codegen/TypeLayout.h"
#include "dialect/SammineDialect.h"

namespace sammine_lang {

std::optional<llvm::SmallVector<mlir::Type>>
MIRTypes::mirTypes(llvm::ArrayRef<Type> types) const {
  llvm::SmallVector<mlir::Type> mir;
  for (auto &type : types) {
    auto converted = mirType(type);
    if (!converted)
      return std::nullopt;
    mir.push_back(*converted);
  }
  return mir;
}

std::optional<mlir::Type> MIRTypes::mirType(const Type &type) const {
  switch (type.type_kind) {
  case TypeKind::I32_t:
  case TypeKind::U32_t:
  case TypeKind::Integer: // an untyped integer literal is an i32
    return mlir::IntegerType::get(ctx_, 32);
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return mlir::IntegerType::get(ctx_, 64);
  case TypeKind::F32_t:
    return mlir::Float32Type::get(ctx_);
  case TypeKind::F64_t:
  case TypeKind::Flt: // an untyped float literal is an f64
    return mlir::Float64Type::get(ctx_);
  case TypeKind::Bool:
    return mlir::IntegerType::get(ctx_, 1);
  case TypeKind::Char:
    return mlir::IntegerType::get(ctx_, 8);
  case TypeKind::Pointer: {
    // 'ptr<T> owns what it points at; ptr<T> is a view of it.
    auto pointee = mirType(std::get<PointerType>(type.type_data).get_pointee());
    if (!pointee)
      return std::nullopt;
    return smn::PtrType::get(ctx_, *pointee,
                             type.linearity == Linearity::Linear
                                 ? smn::Ownership::Owned
                                 : smn::Ownership::View);
  }
  case TypeKind::Struct: {
    // No type refers to itself, through a pointer or not: the type checker
    // reports one that does.
    auto &st = std::get<StructType>(type.type_data);
    auto fields = mirTypes(st.get_field_types());
    if (!fields)
      return std::nullopt;
    llvm::SmallVector<mlir::StringAttr> names;
    for (auto &field : st.get_field_names())
      names.push_back(mlir::StringAttr::get(ctx_, field));
    return smn::StructType::get(ctx_, st.get_name().mangled(), names, *fields);
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(type.type_data);
    // An integer-backed enum is its backing integer.
    if (et.is_integer_backed()) {
      auto backing = et.get_backing_type();
      return mlir::IntegerType::get(
          ctx_,
          backing == TypeKind::I64_t || backing == TypeKind::U64_t ? 64 : 32);
    }
    llvm::SmallVector<mlir::Type> variants;
    int64_t payloadSize = 0;
    for (auto &variant : et.get_variants()) {
      auto fields = mirTypes(variant.payload_types);
      if (!fields)
        return std::nullopt;
      variants.push_back(smn::TupleType::get(ctx_, *fields));
      payloadSize = std::max(payloadSize,
                             layout::variantPayloadSize(variant.payload_types));
    }
    llvm::SmallVector<mlir::StringAttr> names;
    for (auto &variant : et.get_variants())
      names.push_back(mlir::StringAttr::get(ctx_, variant.name));
    return smn::EnumType::get(ctx_, et.get_name().mangled(), payloadSize,
                              names, variants);
  }
  case TypeKind::Function: {
    auto &fn = std::get<FunctionType>(type.type_data);
    auto signature = mirFunctionType(fn);
    if (fn.is_var_arg() || !signature)
      return std::nullopt;
    return smn::FnType::get(ctx_, *signature);
  }
  case TypeKind::Array: {
    auto &array = std::get<ArrayType>(type.type_data);
    auto element = mirType(array.get_element());
    if (!element)
      return std::nullopt;
    return smn::ArrayType::get(ctx_, static_cast<int64_t>(array.get_size()),
                               *element);
  }
  case TypeKind::Tuple: {
    auto elements =
        mirTypes(std::get<TupleType>(type.type_data).get_element_types());
    if (!elements)
      return std::nullopt;
    return smn::TupleType::get(ctx_, *elements);
  }
  default:
    return std::nullopt;
  }
}

std::optional<mlir::FunctionType>
MIRTypes::mirFunctionType(const FunctionType &fn) const {
  auto inputs = mirTypes(fn.get_params_types());
  if (!inputs)
    return std::nullopt;
  if (fn.get_return_type().type_kind == TypeKind::Unit)
    return mlir::FunctionType::get(ctx_, *inputs, {});
  auto result = mirType(fn.get_return_type());
  if (!result)
    return std::nullopt;
  return mlir::FunctionType::get(ctx_, *inputs, {*result});
}

} // namespace sammine_lang
