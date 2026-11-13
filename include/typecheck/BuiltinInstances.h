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

/// The typeclass whose instance for a type drops a value of it: the drops
/// the compiler inserts call `Drop<T>::Drop`.
inline constexpr std::string_view kDropClass = "Drop";

/// The typeclass `x as T` goes through, as `Convert<From, T>`.
inline constexpr std::string_view kConvertClass = "Convert";

/// True if `Convert<from, to>` is built in: between any two number types,
/// between char and the integer types, and between a string and a view of
/// its chars. Like C, a conversion to a narrower
/// integer type keeps the low bits. A float to an integer drops the fraction
/// and saturates: NaN gives 0, and a value out of range the type's min or max.
inline bool is_builtin_conversion(const Type &from, const Type &to) {
  auto is_number = [](const Type &t) {
    return t.is_numeric() && !t.is_polymorphic_numeric();
  };
  auto is_int = [](const Type &t) {
    return t.is_integer() && !t.is_polymorphic_numeric();
  };
  // A string is a pointer to its chars, which a view of them reads; such
  // a view is a string only where an unsafe function says so (the type
  // checker asks, where the conversion is written).
  auto is_char_view = [](const Type &t) {
    return t.type_kind == TypeKind::Pointer && !t.is_linear_v() &&
           std::get<PointerType>(t.type_data).get_pointee().type_kind ==
               TypeKind::Char;
  };
  if ((from.type_kind == TypeKind::String && is_char_view(to)) ||
      (is_char_view(from) && to.type_kind == TypeKind::String))
    return true;
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

/// The typeclass `{}` prints an argument through, as `Format<T>`.
inline constexpr std::string_view kFormatClass = "Format";

/// How C's printf prints a value of `t`, for the types whose `Format<t>`
/// the compiler provides; none for any other. The numbers print as numbers
/// (a float's digits after the point come from the placeholder), a char as
/// itself, a string as its text, and a bool as text too, `true` or `false`.
/// An enum that is an integer prints as it: its values needn't be a variant
/// each, as Read | Write isn't.
inline std::optional<std::string_view> builtin_format_directive(const Type &t) {
  auto kind = t.type_kind;
  if (auto *en = std::get_if<EnumType>(&t.type_data)) {
    if (!en->is_integer_backed())
      return std::nullopt;
    kind = en->get_backing_type();
  }
  switch (kind) {
  case TypeKind::I32_t:
    return "%d";
  case TypeKind::I64_t:
    return "%lld";
  case TypeKind::U32_t:
    return "%u";
  case TypeKind::U64_t:
    return "%llu";
  case TypeKind::F32_t:
  case TypeKind::F64_t:
    return "%f";
  case TypeKind::Char:
    return "%c";
  case TypeKind::Bool:
  case TypeKind::String:
    return "%s";
  default:
    return std::nullopt;
  }
}

/// The types a value of `t` prints part by part, each through its own
/// instance of Format, for the types the compiler prints so: a tuple's
/// elements, between parentheses, and an array's, between brackets. None
/// for any other type.
inline std::optional<std::vector<Type>> format_parts(const Type &t) {
  if (auto *tuple = std::get_if<TupleType>(&t.type_data)) {
    std::vector<Type> parts;
    for (size_t i = 0; i < tuple->size(); ++i)
      parts.push_back(tuple->get_element(i));
    return parts;
  }
  if (auto *array = std::get_if<ArrayType>(&t.type_data))
    return std::vector<Type>{array->get_element()};
  return std::nullopt;
}

/// True if the compiler provides the instance `key`.
inline bool is_builtin_instance(const MonomorphizedKey &key) {
  // Only the prelude's classes have them: a module's own Add is its own.
  if (!key.name.is_of_prelude())
    return false;
  if (key.is_of_prelude_class(kConvertClass))
    return key.type_args.size() == 2 &&
           is_builtin_conversion(key.type_args[0], key.type_args[1]);
  if (key.is_of_prelude_class(kFormatClass))
    // A tuple's and an array's too, which need their parts' (has_instance).
    return key.type_args.size() == 1 &&
           (builtin_format_directive(key.type_args[0]).has_value() ||
            format_parts(key.type_args[0]).has_value());
  auto op = operator_of_class(key.name.get_name());
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
