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
    {TokADD, "Add"}, {TokSUB, "Sub"}, {TokMUL, "Mul"},
    {TokDIV, "Div"}, {TokMOD, "Mod"},
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

/// True if the compiler provides the instance `key`.
inline bool is_builtin_instance(const MonomorphizedKey &key) {
  auto op = operator_of_class(key.name);
  if (!op || key.type_args.size() != 1)
    return false;
  auto &t = key.type_args[0];
  if (t.is_polymorphic_numeric())
    return false;
  if (*op == TokMOD)
    return t.is_integer();
  return t.is_numeric();
}

} // namespace sammine_lang
