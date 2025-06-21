#pragma once
#include "util/Utilities.h"
#include <cmath>
#include <execution>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
//! \file Types.h
//! \brief Defines the core Type system for Sammine
enum class TypeKind {
  I64_t,
  F64_t,
  Unit,
  Bool,
  String,
  Function,
  NonExistent,
  Poisoned
};

struct Type;
class FunctionType;

using TypePtr = std::shared_ptr<Type>;
class FunctionType {
  std::vector<Type> total_types;

public:
  bool operator==(const FunctionType &t) const;

  bool operator<(const FunctionType &t) const;

  std::span<const Type> get_params_types() const;
  Type get_return_type() const;

  FunctionType(const std::vector<Type> &total_types);
};
using TypeData = std::variant<FunctionType, std::string, std::monostate>;

struct Type {
  TypeKind type_kind;
  TypeData type_data;
  bool is_checked = false;
  // Constructors
  Type() : type_kind(TypeKind::NonExistent), type_data(std::monostate()) {}
  static Type I64_t() { return Type{TypeKind::I64_t, std::monostate()}; }
  static Type F64_t() { return Type{TypeKind::F64_t, std::monostate()}; }
  static Type Bool() { return Type{TypeKind::Bool, std::monostate()}; }
  static Type Poisoned() { return Type{TypeKind::Poisoned, std::monostate()}; }
  static Type Unit() { return Type{TypeKind::Unit, std::monostate()}; }
  static Type String(const std::string &str) {
    return Type{TypeKind::String, str};
  }
  static Type NonExistent() {
    return Type{TypeKind::NonExistent, std::monostate()};
  }
  static Type Function(std::vector<Type> params);
  explicit operator bool() const {
    return this->type_kind != TypeKind::Poisoned;
  }
  bool synthesized() const {
    return this->type_kind != TypeKind::NonExistent &&
           this->type_kind == TypeKind::Poisoned;
  }
  bool checked() const {
    return this->is_checked || this->type_kind == TypeKind::Poisoned;
  }
  Type(TypeKind type_kind, TypeData type_data)
      : type_kind(type_kind), type_data(type_data) {}

  bool operator==(const Type &other) const;

  bool operator!=(const Type &other) const;
  bool operator<(const Type &t) const;
  bool operator>(const Type &t) const;

  std::string to_string() const {
    switch (type_kind) {
    case TypeKind::I64_t:
      return "i64";
    case TypeKind::F64_t:
      return "f64";
    case TypeKind::Unit:
      return "()";
    case TypeKind::Bool:
      return "bool";
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
    case TypeKind::NonExistent:
      return "??";
    case TypeKind::Poisoned:
      return "Poisoned";
    case TypeKind::String:
      return fmt::format("\"{}\"", std::get<std::string>(type_data));
    }
    sammine_util::abort("Reaching the end of switch case and still cant "
                        "convert to string, blame Jasmine (badumbatish)!!!!!");
    return "";
  }

  operator std::string() { return to_string(); }
};

struct TypeMapOrdering {
  std::map<Type, Type> type_map;
  std::vector<Type> visit_ancestor(const Type &t);
  std::optional<Type> lowest_common_type(const Type &a, const Type &b);

  bool compatible_to_from(const Type &a, const Type &b);
};
