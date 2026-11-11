#pragma once

#include "lex/Token.h"
#include "typecheck/Types.h"

#include <optional>
#include <string_view>

//! \file BuiltinInstances.h
//! \brief The typeclass instances the compiler provides without source code,
//! such as Add<i32>. Codegen emits an instruction for them instead of a call.

namespace sammine_lang {

/// An operator that goes through a typeclass, as `+` goes through `Add`. The
/// class has one method, named after the class.
struct OperatorClass {
  TokenType op;
  std::string_view class_name;
};

inline constexpr OperatorClass kOperatorClasses[] = {
    {TokADD, "Add"},
    {TokSUB, "Sub"},
    {TokMUL, "Mul"},
    {TokDIV, "Div"},
    {TokMOD, "Mod"},
    {TokLESS, "Less"},
    {TokGREATER, "Greater"},
    {TokLessEqual, "LessEqual"},
    {TokGreaterEqual, "GreaterEqual"},
    {TokEQUAL, "Equal"},
    {TokNOTEqual, "NotEqual"},
};

/// The typeclass `op` goes through, or nullopt if it has none.
inline std::optional<std::string_view> class_of_operator(TokenType op) {
  for (auto &entry : kOperatorClasses)
    if (entry.op == op)
      return entry.class_name;
  return std::nullopt;
}

/// The operator that goes through `class_name`, or nullopt if none does.
inline std::optional<TokenType> operator_of_class(std::string_view class_name) {
  for (auto &entry : kOperatorClasses)
    if (entry.class_name == class_name)
      return entry.op;
  return std::nullopt;
}

/// The typeclass `x as T` goes through, as `Convert<From, T>`.
inline constexpr std::string_view kConvertClass = "Convert";

/// True if `Convert<from, to>` is built in: between any two number types,
/// and between char and the integer types. Like C, a conversion to a narrower
/// integer type keeps the low bits. A float to an integer drops the fraction
/// and saturates: NaN gives 0, and a value out of range the type's min or max.
inline bool is_builtin_conversion(const Type &from, const Type &to) {
  auto is_number = [](const Type &t) {
    return t.is_numeric() && !t.is_polymorphic_numeric();
  };
  auto is_int = [](const Type &t) {
    return t.is_integer() && !t.is_polymorphic_numeric();
  };
  bool from_char = from.type_kind == TypeKind::Char;
  bool to_char = to.type_kind == TypeKind::Char;
  if (from_char && to_char)
    return true;
  if (from_char)
    return is_int(to);
  if (to_char)
    return is_int(from);
  return is_number(from) && is_number(to);
}

/// True if `==` and `!=` are built in for `t`: numbers (untyped literals
/// too), char, bool, pointers, arrays of those but arrays, and enums whose
/// payloads are those. Two enum values are equal when they are the same
/// variant with equal payloads.
inline bool is_builtin_equality(const Type &t) {
  switch (t.type_kind) {
  case TypeKind::Char:
  case TypeKind::Bool:
  case TypeKind::Pointer:
    return true;
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(t.type_data);
    for (auto &variant : et.get_variants())
      for (auto &payload : variant.payload_types)
        if (!is_builtin_equality(payload))
          return false;
    return true;
  }
  case TypeKind::Array: {
    auto element = std::get<ArrayType>(t.type_data).get_element();
    return element.type_kind != TypeKind::Array &&
           !element.is_polymorphic_numeric() && is_builtin_equality(element);
  }
  default:
    return t.is_numeric();
  }
}

/// True if the compiler provides the instance `key`.
inline bool is_builtin_instance(const MonomorphizedKey &key) {
  if (key.name == kConvertClass)
    return key.type_args.size() == 2 &&
           is_builtin_conversion(key.type_args[0], key.type_args[1]);
  auto op = operator_of_class(key.name);
  if (!op || key.type_args.size() != 1)
    return false;
  auto &t = key.type_args[0];
  switch (*op) {
  case TokEQUAL:
  case TokNOTEqual:
    return is_builtin_equality(t);
  case TokLESS:
  case TokGREATER:
  case TokLessEqual:
  case TokGreaterEqual:
    // Numbers, untyped literals too (1 < 2), and char.
    return t.is_numeric() || t.type_kind == TypeKind::Char;
  default:
    break;
  }
  if (t.is_polymorphic_numeric())
    return false;
  if (*op == TokMOD)
    return t.is_integer();
  return t.is_numeric();
}

} // namespace sammine_lang
