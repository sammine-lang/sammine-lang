#include "typecheck/Types.h"
#include "util/Utilities.h"
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
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
FunctionType::FunctionType(const std::vector<Type> &total_types)
    : total_types(total_types) {}

std::span<const Type> FunctionType::get_params_types() const {
  return std::span<const Type>(total_types.data(), total_types.size() - 1);
}

Type FunctionType::get_return_type() const { return total_types.back(); }

Type Type::Function(std::vector<Type> params) {
  return Type{TypeKind::Function, FunctionType{params}};
}
bool Type::operator!=(const Type &other) const { return !(operator==(other)); }
bool Type::operator<(const Type &t) const { return this->operator==(t); }
bool Type::operator>(const Type &t) const { return this->operator==(t); }
bool Type::operator==(const Type &other) const {
  if (this->type_kind != other.type_kind)
    return false;
  if (this->type_kind != TypeKind::Function)
    return true;

  return this->type_data == other.type_data;
}

std::vector<Type> TypeMapOrdering::visit_ancestor(const Type &t) {
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
                                                        const Type &b) {
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

bool TypeMapOrdering::compatible_to_from(const Type &a, const Type &b) {
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
  return a == b;
}
