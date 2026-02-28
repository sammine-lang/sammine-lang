#pragma once

#include "lex/Token.h"
#include <cstdint>
#include <string_view>

namespace sammine_lang {
namespace cst {

/// Unified syntax kind enum covering both tokens and CST nodes.
/// Tokens occupy [0, 1024), nodes occupy [1024, 2048).
enum class SyntaxKind : uint16_t {
  // ---- Tokens [0, 1024) ----
  // Arithmetic
  TokADD = 0,
  TokSUB,
  TokMUL,
  TokDIV,
  TokMOD,

  // Compound assignment
  TokAddAssign,
  TokAddIncr,
  TokSubAssign,
  TokSubDecr,
  TokMulAssign,
  TokDivAssign,

  // Logical / Bitwise
  TokAND,
  TokAndLogical,
  TokOR,
  TokORLogical,
  TokPipe,
  TokXOR,
  TokSHL,
  TokSHR,

  // Comparison
  TokEQUAL,
  TokLESS,
  TokLessEqual,
  TokGREATER,
  TokGreaterEqual,

  // Assignment
  TokASSIGN,
  TokNOT,
  TokNOTEqual,

  // Exponentiation / floor-ceil div
  TokEXP,
  TokFloorDiv,
  TokCeilDiv,

  // Delimiters
  TokLeftParen,
  TokRightParen,
  TokLeftCurly,
  TokRightCurly,
  TokLeftBracket,
  TokRightBracket,

  // Punctuation
  TokComma,
  TokDot,
  TokSemiColon,
  TokColon,
  TokDoubleColon,

  // Keywords
  TokReturn,
  TokFunc,
  TokStruct,
  TokPtr,
  TokAlloc,
  TokFree,
  TokLen,
  TokArrow,
  TokLet,
  TokMUT,
  TokReuse,
  TokExport,
  TokImport,
  TokAs,
  TokEllipsis,
  TokTypeclass,
  TokInstance,
  TokCase,
  TokFatArrow,

  // Identifiers and literals
  TokID,
  TokStr,
  TokNum,
  TokTrue,
  TokFalse,
  TokChar,
  TokTick,

  // Control flow keywords
  TokIf,
  TokElse,
  TokWhile,

  TokType,

  // Comment (preserved for CST)
  TokSingleComment,

  TokEOF,
  TokINVALID,

  // Trivia tokens (new for CST)
  TokWhitespace,
  TokNewline,

  // ---- Nodes [1024, 2048) ----
  NodeBase = 1024,

  // Top-level
  SourceFile = NodeBase,

  // Definitions
  FuncDef,
  ExternDef,
  StructDef,
  EnumDef,
  TypeAliasDef,
  TypeClassDecl,
  TypeClassInstance,
  ImportDecl,

  // Prototype / params
  Prototype,
  TypedVar,
  ParamList,
  TypeArgList,

  // Type expressions
  SimpleType,
  PtrType,
  LinearPtrType,
  ArrayType,
  FuncType,
  GenericType,
  TupleType,

  // Expressions
  VarDef,
  NumberExpr,
  StringExpr,
  BoolExpr,
  CharExpr,
  BinaryExpr,
  CallExpr,
  ReturnExpr,
  UnitExpr,
  VariableExpr,
  IfExpr,
  DerefExpr,
  AddrOfExpr,
  AllocExpr,
  FreeExpr,
  ArrayLiteralExpr,
  IndexExpr,
  LenExpr,
  UnaryNegExpr,
  StructLiteralExpr,
  FieldAccessExpr,
  CaseExpr,
  CaseArm,
  WhileExpr,
  TupleLiteralExpr,
  ParenExpr,

  // Blocks
  Block,

  // Struct / Enum members
  StructField,
  EnumVariant,

  // Arguments
  ArgList,
  FieldInit,

  // Error recovery node
  Error,
};

/// Returns true if the kind is a token (< 1024)
inline constexpr bool is_token(SyntaxKind kind) {
  return static_cast<uint16_t>(kind) < 1024;
}

/// Returns true if the kind is a node (>= 1024)
inline constexpr bool is_node(SyntaxKind kind) {
  return static_cast<uint16_t>(kind) >= 1024;
}

/// Returns true if the kind is trivia (whitespace, newline, comment)
inline constexpr bool is_trivia(SyntaxKind kind) {
  return kind == SyntaxKind::TokWhitespace || kind == SyntaxKind::TokNewline ||
         kind == SyntaxKind::TokSingleComment;
}

/// Returns a human-readable name for a SyntaxKind
std::string_view syntax_kind_name(SyntaxKind kind);

/// Converts a TokenType to the corresponding SyntaxKind
SyntaxKind from_token_type(TokenType tt);

} // namespace cst
} // namespace sammine_lang
