#include "codegen/TypeLayout.h"

#include "fmt/format.h"
#include "llvm/Support/MathExtras.h"

namespace sammine_lang::layout {

int64_t typeSize(const Type &type) {
  switch (type.type_kind) {
  case TypeKind::I32_t:
  case TypeKind::U32_t:
  case TypeKind::Integer:
    return 4;
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return 8;
  case TypeKind::F64_t:
  case TypeKind::Flt:
    return 8;
  case TypeKind::F32_t:
    return 4;
  case TypeKind::Bool:
  case TypeKind::Char:
    return 1;
  case TypeKind::Unit:
    return 0;
  case TypeKind::Pointer:
  case TypeKind::String:
    return 8;
  case TypeKind::Function:
    return 16; // closure struct: two pointers
  case TypeKind::Array: {
    auto &arr = std::get<ArrayType>(type.type_data);
    return typeSize(arr.get_element()) *
           static_cast<int64_t>(arr.get_size());
  }
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(type.type_data);
    int64_t size = 0;
    for (auto &ft : st.get_field_types()) {
      int64_t fieldSize = typeSize(ft);
      size = static_cast<int64_t>(llvm::alignTo(size, fieldSize));
      size += fieldSize;
    }
    // Align total to largest field alignment
    if (!st.get_field_types().empty()) {
      int64_t maxAlign = 0;
      for (auto &ft : st.get_field_types())
        maxAlign = std::max(maxAlign, typeSize(ft));
      size = static_cast<int64_t>(llvm::alignTo(size, maxAlign));
    }
    return size;
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(type.type_data);
    if (et.is_integer_backed()) {
      auto bt = et.get_backing_type();
      return (bt == TypeKind::I64_t || bt == TypeKind::U64_t) ? 8 : 4;
    }
    int64_t max_payload = 0;
    for (auto &vi : et.get_variants())
      max_payload =
          std::max(max_payload, variantPayloadSize(vi.payload_types));
    return 4 + max_payload; // i32 tag + payload
  }
  case TypeKind::Tuple: {
    auto &tt = std::get<TupleType>(type.type_data);
    int64_t size = 0;
    for (auto &et : tt.get_element_types()) {
      int64_t fieldSize = typeSize(et);
      size = static_cast<int64_t>(llvm::alignTo(size, fieldSize));
      size += fieldSize;
    }
    if (!tt.get_element_types().empty()) {
      int64_t maxAlign = 0;
      for (auto &et : tt.get_element_types())
        maxAlign = std::max(maxAlign, typeSize(et));
      size = static_cast<int64_t>(llvm::alignTo(size, maxAlign));
    }
    return size;
  }
  case TypeKind::NonExistent:
    sammine_util::abort("getTypeSize: type was never resolved (NonExistent) — "
              "this is a compiler bug: a type expression was not "
              "visited during type checking");
  case TypeKind::Poisoned:
    sammine_util::abort("getTypeSize: poisoned type reached codegen — "
              "a type error should have been reported and "
              "compilation halted before codegen");
  case TypeKind::Generic:
    sammine_util::abort("getTypeSize: generic template type reached codegen — "
              "this is a compiler bug: the generic definition was "
              "not monomorphized before codegen");
  case TypeKind::TypeParam:
    sammine_util::abort(fmt::format(
        "getTypeSize: unresolved type parameter '{}' reached codegen — "
        "this is a compiler bug: the type parameter was not substituted "
        "during monomorphization",
        type.to_string()));
  case TypeKind::Never:
    sammine_util::abort("getTypeSize: 'never' type reached codegen — "
              "this is a compiler bug: a diverging expression's "
              "type was not handled before codegen");
  default:
    sammine_util::abort(fmt::format("getTypeSize: unhandled type kind: {}",
                          type.to_string()));
  }
}

int64_t variantPayloadSize(const std::vector<Type> &payload_types) {
  int64_t size = 0;
  for (auto &pt : payload_types) {
    int64_t fieldSize = typeSize(pt);
    size = static_cast<int64_t>(llvm::alignTo(size, fieldSize));
    size += fieldSize;
  }
  return size;
}

int64_t advancePayloadOffset(int64_t &byte_offset, const Type &field_type) {
  int64_t fieldSize = typeSize(field_type);
  byte_offset = static_cast<int64_t>(llvm::alignTo(byte_offset, fieldSize));
  int64_t offset = byte_offset;
  byte_offset += fieldSize;
  return offset;
}

} // namespace sammine_lang::layout
