#include "ast/Ast.h"
#include "lex/Token.h"
#include <sstream>
#include <string>

namespace sammine_lang::AST {

// ── helpers ──────────────────────────────────────────────────────────

static std::string escape_string(const std::string &s) {
  std::string result;
  for (char c : s) {
    switch (c) {
    case '\n': result += "\\n"; break;
    case '\t': result += "\\t"; break;
    case '\\': result += "\\\\"; break;
    case '"':  result += "\\\""; break;
    default:   result += c; break;
    }
  }
  return result;
}

static std::string escape_char(char c) {
  switch (c) {
  case '\n': return "\\n";
  case '\t': return "\\t";
  case '\\': return "\\\\";
  case '\'': return "\\'";
  case '\0': return "\\0";
  default:   return std::string(1, c);
  }
}

// ── TypedVarAST ──────────────────────────────────────────────────────

std::string TypedVarAST::to_string() const {
  std::string result = name;
  if (type_expr)
    result += " : " + type_expr->to_string();
  return result;
}

// ── PrototypeAST ─────────────────────────────────────────────────────

std::string PrototypeAST::to_string() const {
  std::string result = functionName.mangled();
  if (!type_params.empty()) {
    result += "<";
    for (size_t i = 0; i < type_params.size(); i++) {
      result += type_params[i];
      if (i + 1 < type_params.size())
        result += ", ";
    }
    result += ">";
  }
  result += "(";
  for (size_t i = 0; i < parameterVectors.size(); i++) {
    result += parameterVectors[i]->to_string();
    if (i + 1 < parameterVectors.size() || is_var_arg)
      result += ", ";
  }
  if (is_var_arg)
    result += "...";
  result += ")";
  if (return_type_expr)
    result += " -> " + return_type_expr->to_string();
  return result;
}

// ── BlockAST ─────────────────────────────────────────────────────────

std::string BlockAST::to_string() const {
  std::string result;
  for (size_t i = 0; i < Statements.size(); i++) {
    auto &stmt = Statements[i];
    if (auto *ret = llvm::dyn_cast<ReturnExprAST>(stmt.get())) {
      if (ret->is_implicit) {
        // implicit return: emit expression without "return" or trailing ";"
        if (ret->return_expr)
          result += ret->return_expr->to_string();
        else
          result += "()";
      } else {
        result += stmt->to_string() + ";";
      }
    } else if (stmt->is_statement) {
      result += stmt->to_string() + ";";
    } else {
      result += stmt->to_string();
    }
    if (i + 1 < Statements.size())
      result += "\n";
  }
  return result;
}

// ── FuncDefAST ───────────────────────────────────────────────────────

std::string FuncDefAST::to_string() const {
  std::string result;
  if (is_exported)
    result += "export ";
  result += "let " + Prototype->to_string() + " {\n";
  result += Block->to_string() + "\n}";
  return result;
}

// ── ExprAST subclasses ───────────────────────────────────────────────

std::string NumberExprAST::to_string() const { return number; }

std::string StringExprAST::to_string() const {
  return "\"" + escape_string(string_content) + "\"";
}

std::string BoolExprAST::to_string() const { return b ? "true" : "false"; }

std::string CharExprAST::to_string() const {
  return "'" + escape_char(value) + "'";
}

std::string VariableExprAST::to_string() const { return variableName; }

std::string UnitExprAST::to_string() const { return "()"; }

std::string BinaryExprAST::to_string() const {
  return "(" + LHS->to_string() + " " + Op->lexeme + " " + RHS->to_string() +
         ")";
}

std::string ReturnExprAST::to_string() const {
  if (is_implicit) {
    if (return_expr)
      return return_expr->to_string();
    return "()";
  }
  if (return_expr)
    return "return " + return_expr->to_string();
  return "return ()";
}

std::string CallExprAST::to_string() const {
  std::string result = functionName.mangled();
  if (!explicit_type_args.empty()) {
    result += "<";
    for (size_t i = 0; i < explicit_type_args.size(); i++) {
      result += explicit_type_args[i]->to_string();
      if (i + 1 < explicit_type_args.size())
        result += ", ";
    }
    result += ">";
  }
  result += "(";
  for (size_t i = 0; i < arguments.size(); i++) {
    result += arguments[i]->to_string();
    if (i + 1 < arguments.size())
      result += ", ";
  }
  result += ")";
  return result;
}

std::string VarDefAST::to_string() const {
  std::string result = "let ";
  if (is_mutable)
    result += "mut ";
  if (is_tuple_destructure) {
    result += "(";
    for (size_t i = 0; i < destructure_vars.size(); i++) {
      result += destructure_vars[i]->to_string();
      if (i + 1 < destructure_vars.size())
        result += ", ";
    }
    result += ")";
  } else {
    result += TypedVar->to_string();
  }
  result += " = " + Expression->to_string();
  return result;
}

std::string IfExprAST::to_string() const {
  std::string result = "if " + bool_expr->to_string() + " {\n";
  result += thenBlockAST->to_string() + "\n}";
  if (elseBlockAST) {
    result += " else {\n";
    result += elseBlockAST->to_string() + "\n}";
  }
  return result;
}

std::string DerefExprAST::to_string() const {
  return "*" + operand->to_string();
}

std::string AddrOfExprAST::to_string() const {
  return "&" + operand->to_string();
}

std::string AllocExprAST::to_string() const {
  return "alloc<" + type_arg->to_string() + ">(" + operand->to_string() + ")";
}

std::string FreeExprAST::to_string() const {
  return "free(" + operand->to_string() + ")";
}

std::string ArrayLiteralExprAST::to_string() const {
  std::string result = "[";
  for (size_t i = 0; i < elements.size(); i++) {
    result += elements[i]->to_string();
    if (i + 1 < elements.size())
      result += ", ";
  }
  result += "]";
  return result;
}

std::string IndexExprAST::to_string() const {
  return array_expr->to_string() + "[" + index_expr->to_string() + "]";
}

std::string LenExprAST::to_string() const {
  return "len(" + operand->to_string() + ")";
}

std::string DimExprAST::to_string() const {
  return "dim(" + operand->to_string() + ")";
}

std::string UnaryNegExprAST::to_string() const {
  return "-" + operand->to_string();
}

std::string StructLiteralExprAST::to_string() const {
  std::string result = struct_name.mangled() + " { ";
  for (size_t i = 0; i < field_names.size(); i++) {
    result += field_names[i] + ": " + field_values[i]->to_string();
    if (i + 1 < field_names.size())
      result += ", ";
  }
  result += " }";
  return result;
}

std::string FieldAccessExprAST::to_string() const {
  return object_expr->to_string() + "." + field_name;
}

std::string CaseExprAST::to_string() const {
  std::string result = "case " + scrutinee->to_string() + " {\n";
  for (size_t i = 0; i < arms.size(); i++) {
    auto &arm = arms[i];
    if (arm.pattern.is_wildcard) {
      result += "_ => {\n";
    } else if (arm.pattern.is_literal) {
      result += arm.pattern.literal_value + " => {\n";
    } else {
      result += arm.pattern.variant_name.mangled();
      if (!arm.pattern.bindings.empty()) {
        result += "(";
        for (size_t j = 0; j < arm.pattern.bindings.size(); j++) {
          result += arm.pattern.bindings[j];
          if (j + 1 < arm.pattern.bindings.size())
            result += ", ";
        }
        result += ")";
      }
      result += " => {\n";
    }
    result += arm.body->to_string() + "\n}";
    if (i + 1 < arms.size())
      result += ",\n";
  }
  result += "\n}";
  return result;
}

std::string WhileExprAST::to_string() const {
  return "while " + condition->to_string() + " {\n" + body->to_string() +
         "\n}";
}

std::string TupleLiteralExprAST::to_string() const {
  std::string result = "(";
  for (size_t i = 0; i < elements.size(); i++) {
    result += elements[i]->to_string();
    if (i + 1 < elements.size())
      result += ", ";
  }
  result += ")";
  return result;
}

} // namespace sammine_lang::AST
