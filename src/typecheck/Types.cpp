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
                   std::vector<VariantInfo> variants, bool integer_backed)
    : name(std::move(name)), variants(std::move(variants)),
      integer_backed_(integer_backed) {}
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
           a.type_kind == TypeKind::Integer;
  }
  // Polymorphic float literal can flow into any concrete float type
  if (b.type_kind == TypeKind::Flt) {
    return a.type_kind == TypeKind::F64_t || a.type_kind == TypeKind::Flt;
  }

  // Integer-backed enum can flow into its backing type (i32)
  if (b.type_kind == TypeKind::Enum) {
    auto &et = std::get<EnumType>(b.type_data);
    if (et.is_integer_backed() && a.type_kind == TypeKind::I32_t)
      return true;
  }

  return a == b;
}
