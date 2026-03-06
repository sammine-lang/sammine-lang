#include "typecheck/Types.h"
#include "util/Utilities.h"
#include <algorithm>
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

StructType::StructType(sammine_util::QualifiedName name,
                       std::vector<std::string> field_names,
                       std::vector<Type> field_types)
    : name(std::move(name)), field_names(std::move(field_names)),
      field_types(std::move(field_types)) {}
bool StructType::operator==(const StructType &t) const {
  return name.mangled() == t.name.mangled(); // nominal typing
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
// Qualifiers like mutability and linearity are intentionally ignored — use
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
  // Linearity mismatch: both pointers but different linearity
  if (expected.type_kind == TypeKind::Pointer &&
      actual.type_kind == TypeKind::Pointer &&
      expected.linearity != actual.linearity) {
    auto pointee = std::get<PointerType>(expected.type_data)
                       .get_pointee()
                       .to_string();
    if (actual.linearity == Linearity::Linear) {
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
  if (expected.mutability == Mutability::Mutable &&
      actual.mutability != Mutability::Mutable && !actual.is_literal()) {
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

void TypeMapOrdering::populate() {
  // Polymorphic integer narrowing: Integer is parent of all concrete ints
  type_map[Type::I32_t()] = Type::Integer();
  type_map[Type::I64_t()] = Type::Integer();
  type_map[Type::U32_t()] = Type::Integer();
  type_map[Type::U64_t()] = Type::Integer();

  // Polymorphic float narrowing: Flt is parent of all concrete floats
  type_map[Type::F32_t()] = Type::Flt();
  type_map[Type::F64_t()] = Type::Flt();
}

bool TypeMapOrdering::qualifier_compatible(const Type &to,
                                            const Type &from) const {
  // Linearity for pointers: must match exactly
  // ('ptr<T> and ptr<T> have different ownership semantics)
  if (to.type_kind == TypeKind::Pointer && from.type_kind == TypeKind::Pointer) {
    if (to.linearity != from.linearity)
      return false;
  }
  return true;
}

bool TypeMapOrdering::structurally_compatible(const Type &to,
                                               const Type &from) const {
  // Never is compatible with any type (bottom type subtyping rule)
  if (from.type_kind == TypeKind::Never)
    return true;

  if (to.type_kind == TypeKind::NonExistent &&
      from.type_kind != TypeKind::NonExistent)
    return true;

  // Polymorphic numeric: use the lattice.
  // "from" is polymorphic (Integer or Flt) — check if "from" appears in
  // "to"'s ancestor chain (meaning "to" narrows to "from"'s category).
  if (from.is_polymorphic_numeric()) {
    auto ancestors = visit_ancestor(to);
    return std::any_of(ancestors.begin(), ancestors.end(),
                       [&](const Type &t) { return t == from; });
  }

  // Integer-backed enum can flow into its backing type
  if (from.type_kind == TypeKind::Enum) {
    auto &et = std::get<EnumType>(from.type_data);
    if (et.is_integer_backed() && to.type_kind == et.get_backing_type())
      return true;
  }

  // Tuple compatibility: element-wise, same arity
  if (to.type_kind == TypeKind::Tuple && from.type_kind == TypeKind::Tuple) {
    auto &at = std::get<TupleType>(to.type_data);
    auto &bt = std::get<TupleType>(from.type_data);
    if (at.size() != bt.size())
      return false;
    for (size_t i = 0; i < at.size(); i++) {
      if (!compatible_to_from(at.get_element(i), bt.get_element(i)))
        return false;
    }
    return true;
  }

  return to == from;
}

// Full assignment compatibility: structural match + qualifier checks (mutability, linearity).
// Used for variable initialization, function arguments, and return type validation.
bool TypeMapOrdering::compatible_to_from(const Type &to,
                                          const Type &from) const {
  if (!qualifier_compatible(to, from))
    return false;
  return structurally_compatible(to, from);
}
