#pragma once
#include "util/QualifiedName.h"
#include "util/Utilities.h"
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>
//! \file Types.h
//! \brief Defines the core Type system for Sammine
enum class TypeKind {
  I32_t,
  I64_t,
  F64_t,
  Unit,
  Bool,
  Char,
  String,
  Function,
  Pointer,
  Array,
  Struct,
  Enum,
  Never,
  NonExistent,
  Poisoned,
  Integer,
  Flt,
  TypeParam
};

struct Type;
class FunctionType;

using TypePtr = std::shared_ptr<Type>;
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
class PointerType {
  TypePtr pointee;

public:
  bool operator==(const PointerType &t) const;
  Type get_pointee() const;
  PointerType(Type pointee);
};
class ArrayType {
  TypePtr element;
  size_t size;

public:
  bool operator==(const ArrayType &t) const;
  Type get_element() const;
  size_t get_size() const;
  ArrayType(Type element, size_t size);
};
class StructType {
  std::string name;
  std::vector<std::string> field_names;
  std::vector<Type> field_types;

public:
  bool operator==(const StructType &t) const;
  const std::string &get_name() const { return name; }
  const std::vector<std::string> &get_field_names() const {
    return field_names;
  }
  const std::vector<Type> &get_field_types() const { return field_types; }
  std::optional<size_t> get_field_index(const std::string &field) const;
  Type get_field_type(size_t idx) const;
  size_t field_count() const { return field_names.size(); }
  StructType(std::string name, std::vector<std::string> field_names,
             std::vector<Type> field_types);
};
class EnumType {
  sammine_util::QualifiedName name;

public:
  struct VariantInfo {
    std::string name;
    std::vector<Type> payload_types;
  };

private:
  std::vector<VariantInfo> variants;

public:
  bool operator==(const EnumType &t) const;
  const sammine_util::QualifiedName &get_name() const { return name; }
  const std::vector<VariantInfo> &get_variants() const { return variants; }
  std::optional<size_t>
  get_variant_index(const std::string &variant_name) const;
  const VariantInfo &get_variant(size_t idx) const { return variants[idx]; }
  size_t variant_count() const { return variants.size(); }
  EnumType(sammine_util::QualifiedName name, std::vector<VariantInfo> variants);
};
using TypeData = std::variant<FunctionType, PointerType, ArrayType, StructType,
                              EnumType, std::string, std::monostate>;

struct Type {
  TypeKind type_kind;
  TypeData type_data;
  bool is_mutable = false;
  bool is_linear = false;
  // Constructors
  Type() : type_kind(TypeKind::NonExistent), type_data(std::monostate()) {}
  static Type I32_t() { return Type{TypeKind::I32_t, std::monostate()}; }
  static Type I64_t() { return Type{TypeKind::I64_t, std::monostate()}; }
  static Type F64_t() { return Type{TypeKind::F64_t, std::monostate()}; }
  static Type Bool() { return Type{TypeKind::Bool, std::monostate()}; }
  static Type Char() { return Type{TypeKind::Char, std::monostate()}; }
  static Type Poisoned() { return Type{TypeKind::Poisoned, std::monostate()}; }
  static Type Unit() { return Type{TypeKind::Unit, std::monostate()}; }
  static Type Never() { return Type{TypeKind::Never, std::monostate()}; }
  static Type Integer() { return Type{TypeKind::Integer, std::monostate()}; }
  static Type Flt() { return Type{TypeKind::Flt, std::monostate()}; }
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
  static Type Struct(std::string name, std::vector<std::string> field_names,
                     std::vector<Type> field_types) {
    return Type{TypeKind::Struct,
                StructType(std::move(name), std::move(field_names),
                           std::move(field_types))};
  }
  static Type Enum(sammine_util::QualifiedName name,
                   std::vector<EnumType::VariantInfo> variants) {
    return Type{TypeKind::Enum, EnumType(std::move(name), std::move(variants))};
  }
  static Type Function(std::vector<Type> params, bool var_arg = false);
  explicit operator bool() const {
    return this->type_kind != TypeKind::Poisoned;
  }
  bool synthesized() const {
    return this->type_kind != TypeKind::NonExistent ||
           this->type_kind == TypeKind::Poisoned;
  }
  Type(TypeKind type_kind, TypeData type_data)
      : type_kind(type_kind), type_data(type_data) {}

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
    case TypeKind::F64_t:
      return "f64";
    case TypeKind::Unit:
      return "()";
    case TypeKind::Struct:
      return std::get<StructType>(type_data).get_name();
    case TypeKind::Enum:
      return std::get<EnumType>(type_data).get_name().display();
    case TypeKind::Bool:
      return "bool";
    case TypeKind::Char:
      return "char";
    case TypeKind::Pointer:
      return (is_linear ? "'" : "") + std::string("ptr<") +
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
    case TypeKind::Never:
      return "!";
    case TypeKind::NonExistent:
      return "??";
    case TypeKind::Poisoned:
      return "Poisoned";
    case TypeKind::String:
      return fmt::format("\"{}\"", std::get<std::string>(type_data));
    case TypeKind::Integer:
      return "Integer";
    case TypeKind::Flt:
      return "Flt";
    case TypeKind::TypeParam:
      return std::get<std::string>(type_data);
    }
    sammine_util::abort("Reaching the end of switch case and still cant "
                        "convert to string, blame Jasmine (badumbatish)!!!!!");
  }

  bool is_poisoned() const { return this->type_kind == TypeKind::Poisoned; }

  bool is_literal() const {
    switch (type_kind) {
    case TypeKind::I32_t:
    case TypeKind::I64_t:
    case TypeKind::F64_t:
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

  bool is_polymorphic_numeric() const {
    return type_kind == TypeKind::Integer || type_kind == TypeKind::Flt;
  }

  bool isTypeWrapping() const {
    switch (type_kind) {
    case TypeKind::Pointer:
    case TypeKind::Array:
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::Function:
      return true;
    default:
      return false;
    }
  }

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
    default:
      break;
    }
  }

  operator std::string() const { return to_string(); }
};

struct TypeMapOrdering {
  // TODO: Planned for future subtyping support — don't remove
  std::map<Type, Type> type_map;
  // TODO: Planned for future subtyping support — don't remove
  std::vector<Type> visit_ancestor(const Type &t) const;
  // TODO: Planned for future subtyping support — don't remove
  std::optional<Type> lowest_common_type(const Type &a, const Type &b) const;

  bool compatible_to_from(const Type &a, const Type &b) const;
};
