#include "typecheck/Types.h"
#include "util/Utilities.h"
#include <set>
#include <span>
//! \file Types.cpp
//! \brief Implements the core Type system for Sammine
bool FunctionType::operator==(const FunctionType &t) const {
  return param_types == t.param_types && *return_type == *t.return_type;
}
FunctionType::FunctionType(std::vector<Type> param_types, Type return_type,
                           bool var_arg)
    : param_types(std::move(param_types)),
      return_type(std::make_shared<Type>(std::move(return_type))),
      var_arg(var_arg) {}
FunctionType::FunctionType(const std::vector<Type> &total_types, bool var_arg)
    : param_types(total_types.begin(), total_types.end() - 1),
      return_type(std::make_shared<Type>(total_types.back())),
      var_arg(var_arg) {}

std::span<const Type> FunctionType::get_params_types() const {
  return std::span<const Type>(param_types);
}

Type FunctionType::get_return_type() const { return *return_type; }

PointerType::PointerType(Type pointee)
    : pointee(std::make_shared<Type>(std::move(pointee))) {}
bool PointerType::operator==(const PointerType &t) const {
  return *pointee == *t.pointee;
}
Type PointerType::get_pointee() const { return *pointee; }

ArrayType::ArrayType(Type element, size_t size)
    : element(std::make_shared<Type>(std::move(element))), size(size) {}
bool ArrayType::operator==(const ArrayType &t) const {
  return *element == *t.element && size == t.size;
}
Type ArrayType::get_element() const { return *element; }
size_t ArrayType::get_size() const { return size; }

StructType::StructType(std::string name, std::vector<std::string> field_names,
                       std::vector<Type> field_types)
    : name(std::move(name)), field_names(std::move(field_names)),
      field_types(std::move(field_types)) {}
bool StructType::operator==(const StructType &t) const {
  return name == t.name; // nominal typing
}
std::optional<size_t>
StructType::get_field_index(const std::string &field) const {
  for (size_t i = 0; i < field_names.size(); i++) {
    if (field_names[i] == field)
      return i;
  }
  return std::nullopt;
}
Type StructType::get_field_type(size_t idx) const { return field_types[idx]; }

EnumType::EnumType(sammine_util::QualifiedName name,
                   std::vector<VariantInfo> variants, bool integer_backed,
                   TypeKind backing_type)
    : name(std::move(name)), variants(std::move(variants)),
      integer_backed_(integer_backed), backing_type_(backing_type) {}
bool EnumType::operator==(const EnumType &t) const {
  return name.mangled() == t.name.mangled(); // nominal typing
}
std::optional<size_t>
EnumType::get_variant_index(const std::string &variant_name) const {
  for (size_t i = 0; i < variants.size(); i++) {
    if (variants[i].name == variant_name)
      return i;
  }
  return std::nullopt;
}

TupleType::TupleType(std::vector<Type> element_types)
    : element_types(
          std::make_shared<std::vector<Type>>(std::move(element_types))) {}
bool TupleType::operator==(const TupleType &t) const {
  return *element_types == *t.element_types;
}
const std::vector<Type> &TupleType::get_element_types() const {
  return *element_types;
}
size_t TupleType::size() const { return element_types->size(); }
Type TupleType::get_element(size_t idx) const {
  return (*element_types)[idx];
}

Type::Type(const Type &other) = default;
Type::Type(Type &&other) noexcept = default;
Type &Type::operator=(const Type &other) = default;
Type &Type::operator=(Type &&other) noexcept = default;
Type::~Type() = default;

Type Type::Function(std::vector<Type> params, bool var_arg) {
  return Type{TypeKind::Function, FunctionType{params, var_arg}};
}
bool Type::operator!=(const Type &other) const { return !(operator==(other)); }
bool Type::operator<(const Type &t) const {
  if (type_kind != t.type_kind)
    return static_cast<int>(type_kind) < static_cast<int>(t.type_kind);
  if (type_kind == TypeKind::TypeParam || type_kind == TypeKind::String)
    return std::get<std::string>(type_data) < std::get<std::string>(t.type_data);
  // Same TypeKind with no sub-ordering (scalars, compounds) — equal for ordering
  return false;
}
bool Type::operator>(const Type &t) const { return t < *this; }
// Compares fundamental type structure only (TypeKind + TypeData).
// Qualifiers like is_mutable and is_linear are intentionally ignored — use
// compatible_to_from() for directional compatibility checks.
bool Type::operator==(const Type &other) const {
  if (this->type_kind != other.type_kind)
    return false;
  if (this->type_kind == TypeKind::Function ||
      this->type_kind == TypeKind::Pointer ||
      this->type_kind == TypeKind::Array ||
      this->type_kind == TypeKind::Struct ||
      this->type_kind == TypeKind::Enum ||
      this->type_kind == TypeKind::Tuple ||
      this->type_kind == TypeKind::TypeParam)
    return this->type_data == other.type_data;

  return true;
}

std::vector<Type> TypeMapOrdering::visit_ancestor(const Type &t) const {
  std::vector<Type> result{t};
  std::set<Type> visited{t};
  while (true) {
    auto it = type_map.find(result.back());
    if (it == type_map.end())
      break;

    if (visited.find(it->second) != visited.end()) {
      sammine_util::abort("Cycle detected in type map, ping a dev to turn this "
                          "into dedicated error");
    }
    visited.insert(it->second);
    result.push_back(it->second);
  }
  return result;
}

std::optional<Type> TypeMapOrdering::lowest_common_type(const Type &a,
                                                        const Type &b) const {
  auto list_ancestors_of_a = visit_ancestor(a);
  auto list_ancestors_of_b = visit_ancestor(b);
  auto set_ancestors_of_b =
      std::set(list_ancestors_of_b.begin(), list_ancestors_of_b.end());

  for (auto &ancestor : list_ancestors_of_a) {
    if (set_ancestors_of_b.find(ancestor) != set_ancestors_of_b.end()) {
      return ancestor;
    }
  }
  return std::nullopt;
}

std::optional<std::string> incompatibility_hint(const Type &expected,
                                                const Type &actual) {
  // Linearity mismatch: both pointers but different is_linear
  if (expected.type_kind == TypeKind::Pointer &&
      actual.type_kind == TypeKind::Pointer &&
      expected.is_linear != actual.is_linear) {
    auto pointee = std::get<PointerType>(expected.type_data)
                       .get_pointee()
                       .to_string();
    if (actual.is_linear) {
      return fmt::format(
          "note: 'ptr<{}>' is a linear (owned) pointer, 'ptr<{}>' is "
          "non-linear — these are incompatible",
          pointee, pointee);
    } else {
      return fmt::format(
          "note: 'ptr<{}>' is non-linear, 'ptr<{}>' is a linear (owned) "
          "pointer — these are incompatible",
          pointee, pointee);
    }
  }

  // Mutability mismatch
  if (expected.is_mutable && !actual.is_mutable && !actual.is_literal()) {
    return "note: expected a mutable value, but got an immutable one";
  }

  // Signed/unsigned mismatch
  bool expected_signed = expected.type_kind == TypeKind::I32_t ||
                         expected.type_kind == TypeKind::I64_t;
  bool expected_unsigned = expected.type_kind == TypeKind::U32_t ||
                           expected.type_kind == TypeKind::U64_t;
  bool actual_signed =
      actual.type_kind == TypeKind::I32_t || actual.type_kind == TypeKind::I64_t;
  bool actual_unsigned =
      actual.type_kind == TypeKind::U32_t || actual.type_kind == TypeKind::U64_t;
  if ((expected_signed && actual_unsigned) ||
      (expected_unsigned && actual_signed)) {
    return "note: signed and unsigned integer types cannot be mixed";
  }

  // Tuple arity mismatch
  if (expected.type_kind == TypeKind::Tuple &&
      actual.type_kind == TypeKind::Tuple) {
    auto &et = std::get<TupleType>(expected.type_data);
    auto &at = std::get<TupleType>(actual.type_data);
    if (et.size() != at.size()) {
      return fmt::format(
          "note: tuples have different sizes ({} vs {} elements)", et.size(),
          at.size());
    }
  }

  return std::nullopt;
}

bool TypeMapOrdering::compatible_to_from(const Type &a, const Type &b) const {
  // Never is compatible with any type (bottom type subtyping rule)
  // Since Never represents "no value is ever produced", it can be assigned
  // to any type because the assignment will never actually happen.
  if (b.type_kind == TypeKind::Never) {
    return true;
  }

  if (a.type_kind == TypeKind::NonExistent &&
      b.type_kind != TypeKind::NonExistent) {
    return true;
  }


  // Linearity check: linear and non-linear pointers are incompatible
  if (a.type_kind == TypeKind::Pointer && b.type_kind == TypeKind::Pointer) {
    if (a.is_linear != b.is_linear) {
      return false;
    }
  }

  // Polymorphic integer literal can flow into concrete integer types
  if (b.type_kind == TypeKind::Integer) {
    return a.type_kind == TypeKind::I32_t || a.type_kind == TypeKind::I64_t ||
           a.type_kind == TypeKind::U32_t || a.type_kind == TypeKind::U64_t ||
           a.type_kind == TypeKind::Integer;
  }
  // Polymorphic float literal can flow into any concrete float type
  if (b.type_kind == TypeKind::Flt) {
    return a.type_kind == TypeKind::F64_t || a.type_kind == TypeKind::Flt;
  }

  // Integer-backed enum can flow into its backing type
  if (b.type_kind == TypeKind::Enum) {
    auto &et = std::get<EnumType>(b.type_data);
    if (et.is_integer_backed() && a.type_kind == et.get_backing_type())
      return true;
  }

  // Tuple compatibility: element-wise, same arity
  if (a.type_kind == TypeKind::Tuple && b.type_kind == TypeKind::Tuple) {
    auto &at = std::get<TupleType>(a.type_data);
    auto &bt = std::get<TupleType>(b.type_data);
    if (at.size() != bt.size())
      return false;
    for (size_t i = 0; i < at.size(); i++) {
      if (!compatible_to_from(at.get_element(i), bt.get_element(i)))
        return false;
    }
    return true;
  }

  return a == b;
}
