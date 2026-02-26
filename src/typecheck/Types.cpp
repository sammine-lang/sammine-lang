#include "typecheck/Types.h"
#include "util/Utilities.h"
#include <set>
#include <span>
//! \file Types.cpp
//! \brief Implements the core Type system for Sammine
bool FunctionType::operator<(const FunctionType &t) const {
  if (total_types.size() != t.total_types.size())
    return false;

  for (size_t i = 0; i < total_types.size(); i++) {
    if (!(total_types[i] < t.total_types[i]))
      return false;
  }

  return true;
}
bool FunctionType::operator==(const FunctionType &t) const {
  return total_types == t.total_types;
}
FunctionType::FunctionType(const std::vector<Type> &total_types, bool var_arg)
    : total_types(total_types), var_arg(var_arg) {}

std::span<const Type> FunctionType::get_params_types() const {
  return std::span<const Type>(total_types.data(), total_types.size() - 1);
}

Type FunctionType::get_return_type() const { return total_types.back(); }

PointerType::PointerType(Type pointee)
    : pointee(std::make_shared<Type>(std::move(pointee))) {}
bool PointerType::operator==(const PointerType &t) const {
  return *pointee == *t.pointee;
}
bool PointerType::operator<(const PointerType &t) const {
  return *pointee == *t.pointee;
}
Type PointerType::get_pointee() const { return *pointee; }

ArrayType::ArrayType(Type element, size_t size)
    : element(std::make_shared<Type>(std::move(element))), size(size) {}
bool ArrayType::operator==(const ArrayType &t) const {
  return *element == *t.element && size == t.size;
}
bool ArrayType::operator<(const ArrayType &t) const {
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
bool StructType::operator<(const StructType &t) const {
  return name < t.name;
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
                   std::vector<VariantInfo> variants)
    : name(std::move(name)), variants(std::move(variants)) {}
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
bool Type::operator<(const Type &t) const { return this->operator==(t); }
bool Type::operator>(const Type &t) const { return this->operator==(t); }
// Compares fundamental type structure only (TypeKind + TypeData).
// Qualifiers like is_mutable are intentionally ignored — use
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

  // Mutability check: immutable cannot flow into mutable
  // (mut -> immut is fine, immut -> mut is not)
  // Primitive/literal types bypass this check since they are always by-value.
  if (a.is_mutable && !b.is_mutable && !b.is_literal()) {
    return false;
  }

  return a == b;
}
