#pragma once
#include "util/QualifiedName.h"
#include "util/Utilities.h"
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
//! \file Types.h
//! \brief Defines the core Type system for Sammine
/// Discriminator for all types in the sammine type system.
/// Concrete types (I32_t..Tuple) represent real runtime values.
/// Pseudo-types (Never..Generic) exist only during type checking.
enum class TypeKind {
  // --- Concrete scalar types ---
  I32_t,
  I64_t,
  U32_t,   // unsigned 32-bit — requires explicit suffix (42u32)
  U64_t,   // unsigned 64-bit — requires explicit suffix (100u64)
  F64_t,
  F32_t,   // 32-bit float — suffix syntax: 3.14f32
  Unit,    // void-like type, written as ()
  Bool,
  Char,
  String,
  // --- Compound types (carry TypeData payload) ---
  Function,
  Pointer,
  Array,
  Struct,
  Enum,
  Tuple,
  // --- Pseudo-types (type checker internal, never reach codegen) ---
  Never,       // diverging expressions (e.g. return, abort)
  NonExistent, // not-yet-typed AST node (default before synthesis)
  Poisoned,    // type error sentinel — suppresses cascading errors
  Integer,     // polymorphic integer literal — flows into i32/i64/u32/u64 via context
  Flt,         // polymorphic float literal — flows into f64/f32 via context
  TypeParam,   // generic type parameter (e.g. T in identity<T>)
  Generic      // uninstantiated generic function/struct template
};

struct Type;
class FunctionType;

using TypePtr = std::shared_ptr<Type>;
/// Function type: (param_types) -> return_type, with optional vararg for C FFI.
class FunctionType {
  std::vector<Type> param_types;
  TypePtr return_type; // shared_ptr because Type is incomplete here
  bool var_arg = false;

public:
  bool operator==(const FunctionType &t) const;

  std::span<const Type> get_params_types() const;
  Type get_return_type() const;
  bool is_var_arg() const { return var_arg; }

  FunctionType(std::vector<Type> param_types, Type return_type,
               bool var_arg = false);
  // Legacy: total_types = [params..., return_type]
  FunctionType(const std::vector<Type> &total_types, bool var_arg = false);
};
/// Pointer type: ptr<T> (non-linear) or 'ptr<T> (linear, must be freed).
class PointerType {
  TypePtr pointee;

public:
  bool operator==(const PointerType &t) const;
  Type get_pointee() const;
  PointerType(Type pointee);
};
/// Fixed-size array type: [T; N]. Bounds-checked at runtime.
class ArrayType {
  TypePtr element;
  size_t size;

public:
  bool operator==(const ArrayType &t) const;
  Type get_element() const;
  size_t get_size() const;
  ArrayType(Type element, size_t size);
};
/// Named product type with ordered fields. Name is qualified (e.g. mod::Point).
class StructType {
  sammine_util::QualifiedName name;
  std::vector<std::string> field_names;
  std::vector<Type> field_types;

public:
  bool operator==(const StructType &t) const;
  const sammine_util::QualifiedName &get_name() const { return name; }
  const std::vector<std::string> &get_field_names() const {
    return field_names;
  }
  const std::vector<Type> &get_field_types() const { return field_types; }
  std::optional<size_t> get_field_index(const std::string &field) const;
  Type get_field_type(size_t idx) const;
  size_t field_count() const { return field_names.size(); }
  StructType(sammine_util::QualifiedName name,
             std::vector<std::string> field_names,
             std::vector<Type> field_types);
};
/// Sum type with named variants. Supports unit variants, payload variants,
/// and integer-backed enums (explicit discriminant values, optional backing type).
class EnumType {
  sammine_util::QualifiedName name;

public:
  struct VariantInfo {
    std::string name;
    std::vector<Type> payload_types;          // empty for unit variants
    std::optional<int64_t> discriminant_value; // set for integer-backed enums
  };

private:
  std::vector<VariantInfo> variants;
  bool integer_backed_ = false;
  TypeKind backing_type_ = TypeKind::I32_t;

public:
  bool operator==(const EnumType &t) const;
  const sammine_util::QualifiedName &get_name() const { return name; }
  const std::vector<VariantInfo> &get_variants() const { return variants; }
  std::optional<size_t>
  get_variant_index(const std::string &variant_name) const;
  const VariantInfo &get_variant(size_t idx) const { return variants[idx]; }
  size_t variant_count() const { return variants.size(); }
  bool is_integer_backed() const { return integer_backed_; }
  TypeKind get_backing_type() const { return backing_type_; }
  EnumType(sammine_util::QualifiedName name, std::vector<VariantInfo> variants,
           bool integer_backed = false,
           TypeKind backing_type = TypeKind::I32_t);
};
/// Anonymous product type: (T, U, V). Supports destructuring via let (a, b) = t.
class TupleType {
  std::shared_ptr<std::vector<Type>> element_types;

public:
  bool operator==(const TupleType &t) const;
  const std::vector<Type> &get_element_types() const;
  size_t size() const;
  Type get_element(size_t idx) const;
  TupleType(std::vector<Type> element_types);
};
/// Payload for compound types. std::string holds TypeParam names and String values.
/// std::monostate for scalar types that carry no extra data.
using TypeData = std::variant<FunctionType, PointerType, ArrayType, StructType,
                              EnumType, TupleType, std::string, std::monostate>;

enum class Mutability : uint8_t { Immutable = 0, Mutable = 1 };
enum class Linearity : uint8_t { NonLinear = 0, Linear = 1 }; // Linear = must be consumed exactly once

/// Core type representation. Every AST node gets a Type via ASTProperties.
/// type_kind discriminates, type_data carries compound type details,
/// mutability/linearity are orthogonal qualifiers.
struct Type {
  TypeKind type_kind;
  TypeData type_data;
  Mutability mutability = Mutability::Immutable;
  Linearity linearity = Linearity::NonLinear;

  // Compat accessors for migration
  bool is_mutable_v() const { return mutability == Mutability::Mutable; }
  bool is_linear_v() const { return linearity == Linearity::Linear; }
  // Constructors
  Type() : type_kind(TypeKind::NonExistent), type_data(std::monostate()) {}
  static Type I32_t() { return Type{TypeKind::I32_t, std::monostate()}; }
  static Type I64_t() { return Type{TypeKind::I64_t, std::monostate()}; }
  static Type U32_t() { return Type{TypeKind::U32_t, std::monostate()}; }
  static Type U64_t() { return Type{TypeKind::U64_t, std::monostate()}; }
  static Type F64_t() { return Type{TypeKind::F64_t, std::monostate()}; }
  static Type F32_t() { return Type{TypeKind::F32_t, std::monostate()}; }
  static Type Bool() { return Type{TypeKind::Bool, std::monostate()}; }
  static Type Char() { return Type{TypeKind::Char, std::monostate()}; }
  static Type Poisoned() { return Type{TypeKind::Poisoned, std::monostate()}; }
  static Type Unit() { return Type{TypeKind::Unit, std::monostate()}; }
  static Type Never() { return Type{TypeKind::Never, std::monostate()}; }
  static Type Integer() { return Type{TypeKind::Integer, std::monostate()}; }
  static Type Flt() { return Type{TypeKind::Flt, std::monostate()}; }
  static Type Generic() { return Type{TypeKind::Generic, std::monostate()}; }
  static Type String(const std::string &str) {
    return Type{TypeKind::String, str};
  }
  static Type NonExistent() {
    return Type{TypeKind::NonExistent, std::monostate()};
  }
  static Type TypeParam(const std::string &name) {
    return Type{TypeKind::TypeParam, name};
  }
  static Type Pointer(Type pointee) {
    return Type{TypeKind::Pointer, PointerType(pointee)};
  }
  static Type Array(Type element, size_t size) {
    return Type{TypeKind::Array, ArrayType(element, size)};
  }
  static Type Struct(sammine_util::QualifiedName name,
                     std::vector<std::string> field_names,
                     std::vector<Type> field_types) {
    return Type{TypeKind::Struct,
                StructType(std::move(name), std::move(field_names),
                           std::move(field_types))};
  }
  static Type Enum(sammine_util::QualifiedName name,
                   std::vector<EnumType::VariantInfo> variants,
                   bool integer_backed = false,
                   TypeKind backing_type = TypeKind::I32_t) {
    return Type{TypeKind::Enum,
                EnumType(std::move(name), std::move(variants), integer_backed,
                         backing_type)};
  }
  static Type Tuple(std::vector<Type> element_types) {
    return Type{TypeKind::Tuple, TupleType(std::move(element_types))};
  }
  static Type Function(std::vector<Type> params, bool var_arg = false);
  explicit operator bool() const {
    return this->type_kind != TypeKind::Poisoned;
  }
  bool synthesized() const {
    return this->type_kind != TypeKind::NonExistent;
  }
  Type(TypeKind type_kind, TypeData type_data)
      : type_kind(type_kind), type_data(type_data) {}

  Type(const Type &other);
  Type(Type &&other) noexcept;
  Type &operator=(const Type &other);
  Type &operator=(Type &&other) noexcept;
  ~Type();

  bool operator==(const Type &other) const;

  bool operator!=(const Type &other) const;
  bool operator<(const Type &t) const;
  bool operator>(const Type &t) const;

  std::string to_string() const {
    switch (type_kind) {
    case TypeKind::I32_t:
      return "i32";
    case TypeKind::I64_t:
      return "i64";
    case TypeKind::U32_t:
      return "u32";
    case TypeKind::U64_t:
      return "u64";
    case TypeKind::F64_t:
      return "f64";
    case TypeKind::F32_t:
      return "f32";
    case TypeKind::Unit:
      return "()";
    case TypeKind::Struct:
      return std::get<StructType>(type_data).get_name().mangled();
    case TypeKind::Enum:
      return std::get<EnumType>(type_data).get_name().mangled();
    case TypeKind::Bool:
      return "bool";
    case TypeKind::Char:
      return "char";
    case TypeKind::Pointer:
      return (linearity == Linearity::Linear ? "'" : "") + std::string("ptr<") +
             std::get<PointerType>(type_data).get_pointee().to_string() + ">";
    case TypeKind::Array:
      return "[" + std::get<ArrayType>(type_data).get_element().to_string() +
             ";" + std::to_string(std::get<ArrayType>(type_data).get_size()) +
             "]";
    case TypeKind::Function: {
      std::string res = "(";
      auto fn_type = std::get<FunctionType>(type_data);
      auto param = fn_type.get_params_types();
      for (size_t i = 0; i < param.size(); i++) {
        res += param[i].to_string();
        if (i != param.size() - 1)
          res += ", ";
      }
      res += ") -> ";
      res += fn_type.get_return_type().to_string();

      return res;
    }
    case TypeKind::Tuple: {
      std::string res = "(";
      auto &tt = std::get<TupleType>(type_data);
      for (size_t i = 0; i < tt.size(); i++) {
        res += tt.get_element(i).to_string();
        if (i != tt.size() - 1)
          res += ", ";
      }
      res += ")";
      return res;
    }
    case TypeKind::Never:
      return "!";
    case TypeKind::NonExistent:
      return "??";
    case TypeKind::Poisoned:
      return "Poisoned";
    case TypeKind::String:
      return fmt::format("\"{}\"", std::get<std::string>(type_data));
    case TypeKind::Integer:
      return "numeric literal";
    case TypeKind::Flt:
      return "float literal";
    case TypeKind::TypeParam:
      return std::get<std::string>(type_data);
    case TypeKind::Generic:
      return "generic template";
    }
    sammine_util::abort("Reaching the end of switch case and still cant "
                        "convert to string, blame Jasmine (badumbatish)!!!!!");
  }

  bool is_poisoned() const { return this->type_kind == TypeKind::Poisoned; }

  /// Returns true for types that can appear as literal values (scalars + polymorphic).
  bool is_literal() const {
    switch (type_kind) {
    case TypeKind::I32_t:
    case TypeKind::I64_t:
    case TypeKind::U32_t:
    case TypeKind::U64_t:
    case TypeKind::F64_t:
    case TypeKind::F32_t:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Unit:
    case TypeKind::String:
    case TypeKind::Integer:
    case TypeKind::Flt:
      return true;
    case TypeKind::TypeParam:
      return false;
    default:
      return false;
    }
  }

  /// True for unresolved numeric literals (Integer/Flt) that need context to specialize.
  bool is_polymorphic_numeric() const {
    return type_kind == TypeKind::Integer || type_kind == TypeKind::Flt;
  }

  /// True for compound types that contain inner types (used by forEachInnerType).
  bool isTypeWrapping() const {
    switch (type_kind) {
    case TypeKind::Pointer:
    case TypeKind::Array:
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::Function:
    case TypeKind::Tuple:
      return true;
    default:
      return false;
    }
  }

  /// Recursively checks if this type or any nested type has linear ownership.
  bool containsLinear() const {
    if (linearity == Linearity::Linear)
      return true;
    bool found = false;
    forEachInnerType([&](const Type &inner) {
      if (inner.containsLinear())
        found = true;
    });
    return found;
  }

  /// Visits all immediately-nested types (pointee, elements, fields, params, etc.).
  template <typename F> void forEachInnerType(F &&callback) const {
    switch (type_kind) {
    case TypeKind::Pointer: {
      auto &pt = std::get<PointerType>(type_data);
      callback(pt.get_pointee());
      break;
    }
    case TypeKind::Array: {
      auto &at = std::get<ArrayType>(type_data);
      callback(at.get_element());
      break;
    }
    case TypeKind::Struct: {
      auto &st = std::get<StructType>(type_data);
      for (auto &ft : st.get_field_types())
        callback(ft);
      break;
    }
    case TypeKind::Enum: {
      auto &et = std::get<EnumType>(type_data);
      for (auto &variant : et.get_variants())
        for (auto &pt : variant.payload_types)
          callback(pt);
      break;
    }
    case TypeKind::Function: {
      auto &fn = std::get<FunctionType>(type_data);
      for (auto &p : fn.get_params_types())
        callback(p);
      callback(fn.get_return_type());
      break;
    }
    case TypeKind::Tuple: {
      auto &tt = std::get<TupleType>(type_data);
      for (auto &et : tt.get_element_types())
        callback(et);
      break;
    }
    default:
      break;
    }
  }

  operator std::string() const { return to_string(); }
};

std::optional<std::string> incompatibility_hint(const Type &expected,
                                                const Type &actual);

inline constexpr std::array<std::string_view, 10> kBuiltinTypeNames = {
    "i32", "i64", "f32", "f64", "bool", "char", "u32", "u64", "string", "unit"};

inline bool is_builtin_type_name(std::string_view name) {
  for (auto b : kBuiltinTypeNames)
    if (b == name)
      return true;
  return false;
}

/// Type lattice for subtyping and compatibility checks.
/// Maps child types to parent types (e.g. Integer→i32, Flt→f64).
/// Used by the type checker to resolve polymorphic literals and validate assignments.
struct TypeMapOrdering {
  std::map<Type, Type> type_map;

  /// Populate the type lattice with built-in subtype edges
  void populate();

  std::vector<Type> visit_ancestor(const Type &t) const;
  std::optional<Type> lowest_common_type(const Type &a, const Type &b) const;

  /// Full check: structure + qualifiers (use for assignments, args, returns)
  bool compatible_to_from(const Type &to, const Type &from) const;

  /// Structure only: ignores mutability/linearity (use for if/case arm unification)
  bool structurally_compatible(const Type &to, const Type &from) const;

  /// Qualifier check only: mutability + linearity
  bool qualifier_compatible(const Type &to, const Type &from) const;
};
