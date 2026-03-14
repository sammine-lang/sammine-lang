
#include "parser/Parser.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "lex/Token.h"
#include "parser/Combinators.h"
#include <cstddef>
#include <memory>
#include <utility>
//! \file Parser.cpp
//! \brief Implementation for Parser class, it takes in the token stream and
//! converts it into Parsing things suchs as programs, top-level (struct,
//! functions, global variables)

namespace sammine_lang {
using namespace AST;
// Operator precedence table for precedence-climbing binary expression parsing.
// Lower number = lower precedence (binds less tightly).
// Pipe (|>) is lowest so `x |> f |> g` chains left-to-right.
// Assignment is just above pipe. Logical < comparison < shift < additive <
// multiplicative.
static std::map<TokenType, int> binopPrecedence = {
    {TokenType::TokPipe, 1},          // |>  pipe
    {TokenType::TokASSIGN, 2},        // =   assignment
    {TokenType::TokOR, 3},            // |   bitwise OR
    {TokenType::TokORLogical, 4},     // ||  logical OR
    {TokenType::TokAND, 5},           // &   bitwise AND
    {TokenType::TokXOR, 6},           // ^   bitwise XOR
    {TokenType::TokAndLogical, 7},    // &&  logical AND
    {TokenType::TokLESS, 10},         // <   comparison
    {TokenType::TokLessEqual, 10},    // <=
    {TokenType::TokGreaterEqual, 10}, // >=
    {TokenType::TokGREATER, 10},      // >
    {TokenType::TokEQUAL, 10},        // ==
    {TokenType::TokNOTEqual, 10},     // !=
    {TokenType::TokSHL, 15},          // <<  shift
    {TokenType::TokSHR, 15},          // >>
    {TokenType::TokADD, 20},          // +   additive
    {TokenType::TokSUB, 20},          // -
    {TokenType::TokMUL, 40},          // *   multiplicative
    {TokenType::TokDIV, 40},          // /
    {TokenType::TokMOD, 40},          // %
};

int GetTokPrecedence(TokenType tokType) {
  auto prec = binopPrecedence[tokType];
  return prec > 0 ? prec : -1;
}

auto Parser::Parse() -> u<ProgramAST> { return ParseProgram(); }

auto Parser::ParseImport() -> std::optional<AST::ImportDecl> {
  auto import_tok = expect(TokImport);
  if (!import_tok)
    return std::nullopt;

  auto module_id = expectOrError(TokID, "Expected module name after 'import'",
                                 import_tok->get_location());
  if (!module_id)
    return std::nullopt;

  AST::ImportDecl decl;
  decl.module_name = module_id->lexeme;

  // 'as <alias>' is optional; when omitted, externs are injected directly
  auto as_tok = expect(TokAs);
  if (as_tok) {
    auto alias_id = expectOrError(TokID, "Expected alias after 'as' in import",
                                  as_tok->get_location());
    if (!alias_id)
      return std::nullopt;
    decl.alias = alias_id->lexeme;
    decl.location = import_tok->get_location() | alias_id->get_location();
    alias_to_module[decl.alias] = decl.module_name;
  } else {
    decl.alias = "";
    decl.location = import_tok->get_location() | module_id->get_location();
    // Auto-alias: `import math;` allows `math::add` syntax
    alias_to_module[decl.module_name] = decl.module_name;
  }

  if (!expect(TokSemiColon))
    imm_error("Missing ';' after import statement", decl.location);
  return decl;
}

auto Parser::ParseProgram() -> u<ProgramAST> {
  auto programAST = std::make_unique<ProgramAST>();

  // Parse imports at the top of the file
  while (!tokStream->isEnd() && tokStream->peek()->tok_type == TokImport) {
    auto import_decl = ParseImport();
    if (import_decl)
      programAST->imports.push_back(std::move(*import_decl));
  }

  while (!tokStream->isEnd()) {
    auto [def, result] = ParseDefinition();
    if (result == SUCCESS) {
      programAST->DefinitionVec.push_back(std::move(def));
      continue;
    }
    if (result == NONCOMMITTED)
      break;
    programAST->DefinitionVec.push_back(std::move(def));
    return programAST;
  }
  if (!tokStream->isEnd())
    imm_error("Unexpected token — expected 'let', 'struct', 'type', "
              "'import', 'reuse', or 'typeclass'",
              tokStream->currentLocation());
  return programAST;
}

// Try each definition parser in order. Each returns NONCOMMITTED if the leading
// token doesn't match (e.g. no 'struct' keyword), allowing fallthrough to next.
// 'export' prefix is handled here before dispatching to sub-parsers.
auto Parser::ParseDefinition() -> p<DefinitionAST> {
  // Handle 'export' prefix
  auto export_tok = expect(TokenType::TokExport);
  bool is_exported = export_tok != nullptr;

  auto result = tryParsers<DefinitionAST>(
      &Parser::ParseTypeClassDecl, &Parser::ParseTypeClassInstance,
      &Parser::ParseKernelDef, &Parser::ParseStructDef, &Parser::ParseEnumDef,
      &Parser::ParseReuseDef, &Parser::ParseFuncDef);

  if (is_exported && result.uncommitted()) {
    imm_error("Expected 'let', 'struct', 'type', or 'reuse' after 'export'",
              export_tok->get_location());
    result.status = FAILED;
  }

  if (is_exported && result.node)
    result.node->is_exported = true;

  return result;
}

auto Parser::ParseStructDef() -> p<DefinitionAST> {
  auto struct_tok = expect(TokStruct);
  if (!struct_tok)
    return {nullptr, NONCOMMITTED};

  auto id = expectOrError(TokID, "Expected an identifier after 'struct'",
                          struct_tok->get_location());
  if (!id)
    return {nullptr, FAILED};

  auto struct_pqn = parseQualifiedNameTail(id, false);

  // Parse optional type parameters: struct Pair<T, U> { ... };
  bool tp_error = false;
  auto struct_type_params = parseTypeParams(id->get_location(), tp_error);
  if (tp_error)
    return {nullptr, FAILED};

  auto left_curly =
      expectOrError(TokLeftCurly,
                    fmt::format("Expected '{{' after struct identifier {}",
                                struct_pqn.qn.get_name()),
                    struct_tok->get_location() | struct_pqn.location);
  if (!left_curly)
    return {nullptr, FAILED};

  auto [struct_members, members_error] =
      comma_sep_recover<TypedVarAST>(*this, [&]() { return ParseTypedVar(); });

  auto right_curly = expect(TokRightCurly);
  if (!right_curly) {
    auto err_loc = struct_members.empty()
                       ? left_curly->get_location()
                       : struct_members.back()->get_location();
    auto msg = struct_members.empty()
                   ? fmt::format("Expected '}}' to close struct '{}'",
                                 struct_tok->lexeme)
                   : fmt::format(
                         "Expected '}}' to close struct '{}' after member '{}'",
                         struct_tok->lexeme, struct_members.back()->name);
    imm_error(msg, err_loc);

    return {std::make_unique<StructDefAST>(struct_pqn.qn, struct_pqn.location,
                                           std::move(struct_members)),
            FAILED};
  }
  if (!expect(TokSemiColon))
    imm_error("Missing ';' — struct definitions must end with '};'",
              right_curly->get_location());

  auto struct_def = std::make_unique<StructDefAST>(
      struct_pqn.qn, struct_pqn.location, std::move(struct_members));
  struct_def->type_params = std::move(struct_type_params);
  return {std::move(struct_def), SUCCESS};
}

// type Name = Variant1(Type) | Variant2 | Variant3(Type, Type);
// type Name = ExistingType;  (type alias)
auto Parser::ParseEnumDef() -> p<DefinitionAST> {
  auto type_tok = expect(TokType);
  if (!type_tok)
    return {nullptr, NONCOMMITTED};

  auto id = expectOrError(TokID, "Expected an identifier after 'type'",
                          type_tok->get_location());
  if (!id)
    return {nullptr, FAILED};

  auto enum_pqn = parseQualifiedNameTail(id, false);

  // Parse optional type parameters: type Option<T> = ...
  bool tp_error = false;
  auto enum_type_params = parseTypeParams(id->get_location(), tp_error);
  if (tp_error)
    return {nullptr, FAILED};

  // Parse optional backing type: type Foo: u32 = ...
  std::optional<std::string> backing_type_name;
  if (expect(TokColon)) {
    auto bt_tok = expectOrError(TokID, "Expected backing type name after ':'",
                                id->get_location());
    if (!bt_tok)
      return {nullptr, FAILED};
    backing_type_name = bt_tok->lexeme;
  }

  auto _eq = expectOrError(
      TokASSIGN, fmt::format("Expected '=' after type name '{}'", id->lexeme),
      id->get_location());
  if (!_eq)
    return {nullptr, FAILED};

  // Disambiguate: enum vs type alias
  // If next tokens are ID followed by | or ID followed by ( → enum
  // Otherwise → type alias
  bool is_enum = false;
  if (tokStream->peek()->tok_type == TokID) {
    tokStream->mark_rollback();
    tokStream->consume(); // consume the ID
    auto next = tokStream->peek();
    if (next->tok_type == TokORLogical || next->tok_type == TokLeftParen) {
      // Enum: ID | ... or ID(...)
      is_enum = true;
    }
    tokStream->rollback(); // always restore — just peeking ahead
  }

  // Type alias path
  if (!is_enum) {
    auto type_expr = ParseTypeExpr();
    if (!type_expr) {
      imm_error("Expected type expression in type alias", id->get_location());
      return {nullptr, FAILED};
    }
    if (!expect(TokSemiColon))
      imm_error("Missing ';' — type aliases must end with ';'",
                id->get_location());
    auto alias = std::make_unique<TypeAliasDefAST>(id, std::move(type_expr));
    return {std::move(alias), SUCCESS};
  }

  // Enum path: parse variants separated by |
  std::vector<EnumVariantDef> variants;
  while (!tokStream->isEnd()) {
    auto variant_id = expectOrError(
        TokID, "Expected variant name in type definition",
        variants.empty() ? id->get_location() : variants.back().location);
    if (!variant_id)
      return {std::make_unique<EnumDefAST>(enum_pqn.qn, enum_pqn.location,
                                           std::move(variants)),
              FAILED};

    EnumVariantDef variant;
    variant.name = variant_id->lexeme;
    variant.location = variant_id->get_location();

    // Optional payload: (Type, Type, ...) or (integer_literal)
    if (expect(TokLeftParen)) {
      if (tokStream->peek()->tok_type != TokRightParen) {
        // Negative discriminant: Foo(-1) — not supported
        if (tokStream->peek()->tok_type == TokSUB) {
          imm_error("Negative discriminant values are not supported in "
                    "integer-backed enums",
                    tokStream->peek()->get_location());
          return {std::make_unique<EnumDefAST>(enum_pqn.qn, enum_pqn.location,
                                               std::move(variants)),
                  FAILED};
        }
        // Integer discriminant: Foo(42)
        if (tokStream->peek()->tok_type == TokNum) {
          auto num_tok = expect(TokNum);
          size_t pos = 0;
          int64_t val = std::stoll(num_tok->lexeme, &pos);
          if (pos != num_tok->lexeme.size()) {
            imm_error(
                fmt::format("Expected integer discriminant in variant '{}', "
                            "got '{}'",
                            variant.name, num_tok->lexeme),
                num_tok->get_location());
            return {std::make_unique<EnumDefAST>(enum_pqn.qn, enum_pqn.location,
                                                 std::move(variants)),
                    FAILED};
          }
          variant.discriminant_value = val;
        } else {
          // Type payload: Foo(i32, i32)
          auto [types, ok] = comma_separated<TypeExprAST>(
              [&] { return ParseTypeExpr(); }, *this);
          if (!ok || types.empty()) {
            imm_error(fmt::format("Expected type in payload of variant '{}'",
                                  variant.name),
                      variant.location);
            return {std::make_unique<EnumDefAST>(enum_pqn.qn, enum_pqn.location,
                                                 std::move(variants)),
                    FAILED};
          }
          variant.payload_types = std::move(types);
        }
      }
      auto _rp = expectOrError(
          TokRightParen,
          fmt::format("Expected ')' to close payload of variant '{}'",
                      variant.name),
          variant.location);
      if (!_rp)
        return {std::make_unique<EnumDefAST>(enum_pqn.qn, enum_pqn.location,
                                             std::move(variants)),
                FAILED};
    }

    variants.push_back(std::move(variant));

    // | means more variants, otherwise done
    if (!expect(TokORLogical))
      break;
  }

  if (!expect(TokSemiColon))
    imm_error("Missing ';' — enum definitions must end with ';'",
              variants.empty() ? id->get_location() : variants.back().location);

  auto enum_def = std::make_unique<EnumDefAST>(enum_pqn.qn, enum_pqn.location,
                                               std::move(variants));
  enum_def->type_params = std::move(enum_type_params);
  enum_def->backing_type_name = std::move(backing_type_name);
  // If any variant has an integer discriminant, mark as integer-backed
  for (auto &v : enum_def->variants) {
    if (v.discriminant_value.has_value()) {
      enum_def->is_integer_backed = true;
      break;
    }
  }
  return {std::move(enum_def), SUCCESS};
}

auto Parser::ParseReuseDef() -> p<DefinitionAST> {
  auto reuse_tok = expect(TokenType::TokReuse);
  if (!reuse_tok)
    return {nullptr, NONCOMMITTED};

  auto [prototype, result] =
      ParsePrototypeWithSemi("Missing ';' after reuse declaration");
  if (result != SUCCESS) {
    emit_if_uncommitted(result, "Expected a function prototype after 'reuse'",
                        reuse_tok->get_location());
    (void)expect(TokenType::TokSemiColon);
  }
  return {std::make_unique<ExternAST>(std::move(prototype)),
          result == SUCCESS ? SUCCESS : FAILED};
}

auto Parser::ParseFuncDef() -> p<DefinitionAST> {
  auto fn = expect(TokenType::TokLet);
  if (!fn)
    return {nullptr, NONCOMMITTED};

  auto [prototype, proto_result] = ParsePrototype();
  if (proto_result != SUCCESS) {
    emit_if_uncommitted(proto_result,
                        "Expected a function prototype after 'let'",
                        fn->get_location());
    (void)expect(TokenType::TokRightCurly, /*exhausts=*/true);
    return {std::make_unique<FuncDefAST>(std::move(prototype), nullptr),
            FAILED};
  }
  auto [block, result] = ParseBlock();
  if (result != SUCCESS) {
    emit_if_uncommitted(result, "Expected a function body block",
                        tokStream->currentLocation());
    (void)expect(TokenType::TokRightCurly, /*exhausts=*/true);
    return {
        std::make_unique<FuncDefAST>(std::move(prototype), std::move(block)),
        FAILED};
  }
  return {std::make_unique<FuncDefAST>(std::move(prototype), std::move(block)),
          SUCCESS};
}

//! Parsing implementation for a variable decl/def

//! Accepts a let, continue parsing inside and (enable error reporting if
//! possible). If a `let` is not found then return a nullptr.
auto Parser::ParseVarDef() -> p<ExprAST> {
  auto let = expect(TokenType::TokLet);
  if (!let)
    return {nullptr, NONCOMMITTED};
  auto mut_tok = expect(TokenType::TokMUT);
  bool is_mutable = mut_tok != nullptr;

  // Check for destructuring: let (a, b) = expr;
  if (auto lparen = expect(TokenType::TokLeftParen)) {
    std::vector<std::unique_ptr<TypedVarAST>> vars;
    auto [first_var, first_result] = ParseTypedVar();
    if (first_result != SUCCESS) {
      imm_error("Expected variable name in destructuring pattern",
                lparen->get_location());
      return {nullptr, FAILED};
    }
    vars.push_back(std::move(first_var));
    while (expect(TokenType::TokComma)) {
      auto [next_var, next_result] = ParseTypedVar();
      if (next_result != SUCCESS) {
        imm_error("Expected variable name after ',' in destructuring",
                  lparen->get_location());
        return {nullptr, FAILED};
      }
      vars.push_back(std::move(next_var));
    }
    auto _rp = expectOrError(TokRightParen,
                             "Expected ')' to close destructuring pattern",
                             lparen->get_location());
    if (!_rp)
      return {nullptr, FAILED};
    auto assign_tok = expect(TokenType::TokASSIGN, true, TokSemiColon);
    if (!assign_tok) {
      imm_error("Expected '=' after destructuring pattern",
                lparen->get_location());
      return {nullptr, FAILED};
    }
    auto [expr, result] = ParseExpr();
    if (result != SUCCESS) {
      imm_error("Expected expression in destructuring variable definition",
                let->get_location());
      return {nullptr, FAILED};
    }
    auto semicolon = expect(TokenType::TokSemiColon, true);
    if (!semicolon) {
      imm_error("Missing ';' after variable definition",
                tokStream->currentLocation());
      return {std::make_unique<VarDefAST>(let, std::move(vars), std::move(expr),
                                          is_mutable),
              FAILED};
    }
    return {std::make_unique<VarDefAST>(let, std::move(vars), std::move(expr),
                                        is_mutable),
            SUCCESS};
  }

  auto [typedVar, tv_result] = ParseTypedVar();
  if (tv_result != SUCCESS) {
    emit_if_uncommitted(tv_result, "Expected a typed variable after 'let'",
                        let->get_location());
    return {std::make_unique<VarDefAST>(let, std::move(typedVar), nullptr,
                                        is_mutable),
            FAILED};
  }
  auto assign_tok = expect(TokenType::TokASSIGN, true, TokSemiColon);
  if (!assign_tok) {
    imm_error("Missing '=' in variable definition — syntax is "
              "'let name: type = value;'",
              typedVar->get_location());
    return {std::make_unique<VarDefAST>(let, std::move(typedVar), nullptr,
                                        is_mutable),
            FAILED};
  }
  auto [expr, result] = ParseExpr();
  if (result != SUCCESS) {
    imm_error(result == NONCOMMITTED
                  ? "Expected an expression in variable definition"
                  : "Incomplete expression in variable definition",
              let->get_location());
    return {std::make_unique<VarDefAST>(let, std::move(typedVar),
                                        std::move(expr), is_mutable),
            FAILED};
  }
  auto semicolon = expect(TokenType::TokSemiColon, true);
  if (semicolon)
    return {std::make_unique<VarDefAST>(let, std::move(typedVar),
                                        std::move(expr), is_mutable),
            SUCCESS};
  imm_error("Missing ';' after variable definition",
            tokStream->currentLocation());
  return {std::make_unique<VarDefAST>(let, std::move(typedVar), std::move(expr),
                                      is_mutable),
          FAILED};
}

auto Parser::consumeClosingAngleBracket() -> bool {
  if (tokStream->peek()->tok_type == TokGREATER)
    return tokStream->consume(), true;
  if (tokStream->peek()->tok_type == TokSHR) {
    tokStream->split_current(TokGREATER, ">", TokGREATER, ">");
    tokStream->consume();
    return true;
  }
  if (tokStream->peek()->tok_type == TokGreaterEqual) {
    tokStream->split_current(TokGREATER, ">", TokASSIGN, "=");
    tokStream->consume();
    return true;
  }
  return false;
}

auto Parser::parseTypeParams(Location err_loc, bool &had_error)
    -> std::vector<std::string> {
  std::vector<std::string> params;
  if (!expect(TokLESS))
    return params;
  auto first = expect(TokID);
  if (!first) {
    imm_error("Expected type parameter name after '<'", err_loc);
    had_error = true;
    return params;
  }
  params.push_back(first->lexeme);
  while (expect(TokComma)) {
    auto next = expect(TokID);
    if (!next) {
      imm_error("Expected type parameter name after ','", err_loc);
      had_error = true;
      return params;
    }
    params.push_back(next->lexeme);
  }
  if (!consumeClosingAngleBracket()) {
    imm_error("Expected '>' after type parameter list", err_loc);
    had_error = true;
  }
  return params;
}

auto Parser::ParseTypeExprTopLevel() -> std::unique_ptr<TypeExprAST> {
  return ParseTypeExpr();
}

// Parse a type annotation. Builds a TypeExprAST tree (NOT in the visitor
// pattern). Handles: ptr<T>, 'ptr<T>, [T;N], (T,U)->V, (T,U), Name<T>, and
// simple names.
auto Parser::ParseTypeExpr() -> std::unique_ptr<TypeExprAST> {
  // Parse pointer types: ptr<T> or 'ptr<T> (linear)
  auto tick = expect(TokenType::TokTick);
  if (tick || tokStream->peek()->tok_type == TokenType::TokPtr) {
    auto ptr_tok = expect(TokenType::TokPtr);
    if (!ptr_tok) {
      imm_error("Expected 'ptr' after tick (') for linear pointer type",
                tick->get_location());
      return nullptr;
    }
    auto _lt = expectOrError(TokLESS, "Expected '<' after 'ptr'",
                             ptr_tok->get_location());
    if (!_lt)
      return nullptr;
    auto inner = ParseTypeExpr();
    if (!inner) {
      imm_error("Expected type inside 'ptr<...>'", ptr_tok->get_location());
      return nullptr;
    }
    if (!consumeClosingAngleBracket()) {
      imm_error("Expected '>' to close 'ptr<...>'", ptr_tok->get_location());
      return nullptr;
    }
    bool is_linear = tick != nullptr;
    auto result =
        std::make_unique<PointerTypeExprAST>(std::move(inner), is_linear);
    result->location =
        is_linear ? tick->get_location() : ptr_tok->get_location();
    return result;
  }
  if (auto lbracket = expect(TokenType::TokLeftBracket)) {
    auto size_tok =
        expectOrError(TokNum, "Expected integer size in '[SIZE]TYPE'",
                      lbracket->get_location());
    if (!size_tok)
      return nullptr;
    size_t arr_size = std::stoul(size_tok->lexeme);
    auto _rb = expectOrError(TokRightBracket,
                             "Expected ']' after size in '[SIZE]TYPE'",
                             lbracket->get_location());
    if (!_rb)
      return nullptr;
    auto elem = ParseTypeExpr();
    if (!elem) {
      imm_error("Expected element type after '[SIZE]'",
                lbracket->get_location());
      return nullptr;
    }
    auto result = std::make_unique<ArrayTypeExprAST>(std::move(elem), arr_size);
    result->location = lbracket->get_location();
    return result;
  }
  if (auto lparen = expect(TokenType::TokLeftParen)) {
    // Parse types inside parens: could be function type or tuple type
    std::vector<std::unique_ptr<TypeExprAST>> types;
    if (tokStream->peek()->tok_type != TokenType::TokRightParen) {
      auto [parsed, ok] =
          comma_separated<TypeExprAST>([&] { return ParseTypeExpr(); }, *this);
      if (!ok || parsed.empty()) {
        imm_error("Expected type in parenthesized type expression",
                  lparen->get_location());
        return nullptr;
      }
      types = std::move(parsed);
    }
    auto _rp = expectOrError(TokRightParen, "Expected ')' in type expression",
                             lparen->get_location());
    if (!_rp)
      return nullptr;
    // If '->' follows, it's a function type
    if (expect(TokenType::TokArrow)) {
      auto retType = ParseTypeExpr();
      if (!retType) {
        imm_error("Expected return type after '->' in function type",
                  lparen->get_location());
        return nullptr;
      }
      auto result = std::make_unique<FunctionTypeExprAST>(std::move(types),
                                                          std::move(retType));
      result->location = lparen->get_location();
      return result;
    }
    // Otherwise it's a tuple type (must have 2+ elements)
    if (types.size() >= 2) {
      auto result = std::make_unique<TupleTypeExprAST>(std::move(types));
      result->location = lparen->get_location();
      return result;
    }
    // Single element in parens with no arrow is not valid as a type
    imm_error("Expected '->' for function type or ',' for tuple type",
              lparen->get_location());
    return nullptr;
  }
  if (auto id = expect(TokenType::TokID)) {
    auto pqn = parseQualifiedNameTail(id);
    auto base_name = pqn.qn;
    auto loc = pqn.location;

    // Check for generic type args: Option<i32>, Result<i32, String>
    if (tokStream->peek()->tok_type == TokLESS) {
      tokStream->mark_rollback();
      tokStream->consume(); // consume <
      auto [type_args, ok] =
          comma_separated<TypeExprAST>([&] { return ParseTypeExpr(); }, *this);
      if (ok && !type_args.empty() && consumeClosingAngleBracket()) {
        tokStream->commit_rollback();
        return std::make_unique<GenericTypeExprAST>(base_name,
                                                    std::move(type_args), loc);
      }
      tokStream->rollback();
    }

    return std::make_unique<SimpleTypeExprAST>(base_name, loc);
  }
  return nullptr;
}

auto Parser::ParseTypedVar() -> p<TypedVarAST> {
  auto mut_tok = expect(TokenType::TokMUT);
  bool is_mutable = mut_tok != nullptr;
  auto name = expect(TokenType::TokID);
  if (!name) {
    if (is_mutable) {
      imm_error("Expected identifier after 'mut'", mut_tok->get_location());
      return {std::make_unique<TypedVarAST>(nullptr, nullptr), FAILED};
    }
    return {std::make_unique<TypedVarAST>(nullptr, nullptr), NONCOMMITTED};
  }

  auto colon = expect(TokenType::TokColon);
  if (!colon)
    return {std::make_unique<TypedVarAST>(name, is_mutable), SUCCESS};
  auto type_expr = ParseTypeExprTopLevel();
  if (!type_expr) {
    imm_error("Expected type name after token `:`", colon->get_location());
    return {
        std::make_unique<TypedVarAST>(name, std::move(type_expr), is_mutable),
        FAILED};
  }
  return {std::make_unique<TypedVarAST>(name, std::move(type_expr), is_mutable),
          SUCCESS};
}
template <typename NodeT>
auto Parser::ParseUnaryPrefixExpr(TokenType opTok, const std::string &opStr)
    -> p<ExprAST> {
  auto tok = expect(opTok);
  if (!tok)
    return {nullptr, NONCOMMITTED};
  auto [operand, result] = ParsePrimaryExpr();
  if (result == SUCCESS)
    return {std::make_unique<NodeT>(tok, std::move(operand)), SUCCESS};
  if (result == NONCOMMITTED) {
    imm_error(fmt::format("Expected expression after '{}'", opStr),
              tok->get_location());
    return {nullptr, FAILED};
  }
  return {std::make_unique<NodeT>(tok, std::move(operand)), FAILED};
}

auto Parser::ParseDerefExpr() -> p<ExprAST> {
  std::shared_ptr<Token> star_tok;
  if (pending_deref > 0) {
    pending_deref--;
    star_tok = pending_deref_tok;
  } else {
    star_tok = expect(TokenType::TokMUL);
    if (!star_tok) {
      star_tok = expect(TokenType::TokEXP);
      if (!star_tok)
        return {nullptr, NONCOMMITTED};
      pending_deref++;
      pending_deref_tok = star_tok;
    }
  }
  auto [operand, result] = ParsePrimaryExpr();
  if (result == SUCCESS)
    return {std::make_unique<DerefExprAST>(star_tok, std::move(operand)),
            SUCCESS};
  if (result == NONCOMMITTED) {
    imm_error("Expected expression after '*'", star_tok->get_location());
    return {nullptr, FAILED};
  }
  return {std::make_unique<DerefExprAST>(star_tok, std::move(operand)), FAILED};
}

template <typename NodeT>
auto Parser::ParseBuiltinCallExpr(TokenType keyword, const std::string &name)
    -> p<ExprAST> {
  auto kw_tok = expect(keyword);
  if (!kw_tok)
    return {nullptr, NONCOMMITTED};
  auto left_paren =
      expectOrError(TokLeftParen, fmt::format("Expected '(' after '{}'", name),
                    kw_tok->get_location());
  if (!left_paren)
    return {nullptr, FAILED};
  auto [operand, result] = ParseExpr();
  if (result == NONCOMMITTED) {
    imm_error(fmt::format("Expected expression inside {}()", name),
              kw_tok->get_location());
    return {nullptr, FAILED};
  }
  if (result != SUCCESS) {
    return {std::make_unique<NodeT>(kw_tok, std::move(operand)), FAILED};
  }
  auto _rp = expectOrError(TokRightParen,
                           fmt::format("Expected ')' to close {}()", name),
                           kw_tok->get_location());
  if (!_rp)
    return {std::make_unique<NodeT>(kw_tok, std::move(operand)), FAILED};
  return {std::make_unique<NodeT>(kw_tok, std::move(operand)), SUCCESS};
}

auto Parser::ParseAllocExpr() -> p<ExprAST> {
  auto kw_tok = expect(TokAlloc);
  if (!kw_tok)
    return {nullptr, NONCOMMITTED};

  // Parse <TypeExpr>
  auto _lt = expectOrError(TokLESS, "Expected '<' after 'alloc'",
                           kw_tok->get_location());
  if (!_lt)
    return {nullptr, FAILED};
  auto type_arg = ParseTypeExpr();
  if (!type_arg) {
    imm_error("Expected type in alloc<...>", kw_tok->get_location());
    return {nullptr, FAILED};
  }
  if (!consumeClosingAngleBracket()) {
    imm_error("Expected '>' to close alloc<...>", kw_tok->get_location());
    return {nullptr, FAILED};
  }

  // Parse (count_expr)
  auto _lp = expectOrError(TokLeftParen, "Expected '(' after alloc<T>",
                           kw_tok->get_location());
  if (!_lp)
    return {nullptr, FAILED};
  auto [operand, result] = ParseExpr();
  if (result == NONCOMMITTED) {
    imm_error("Expected expression inside alloc<T>()", kw_tok->get_location());
    return {nullptr, FAILED};
  }
  if (result != SUCCESS)
    return {std::make_unique<AllocExprAST>(kw_tok, std::move(type_arg),
                                           std::move(operand)),
            FAILED};
  auto _rp = expectOrError(TokRightParen, "Expected ')' to close alloc<T>()",
                           kw_tok->get_location());
  if (!_rp)
    return {std::make_unique<AllocExprAST>(kw_tok, std::move(type_arg),
                                           std::move(operand)),
            FAILED};
  return {std::make_unique<AllocExprAST>(kw_tok, std::move(type_arg),
                                         std::move(operand)),
          SUCCESS};
}

auto Parser::ParseArrayLiteralExpr() -> p<ExprAST> {
  auto left_bracket = expect(TokenType::TokLeftBracket);
  if (!left_bracket)
    return {nullptr, NONCOMMITTED};
  std::vector<u<ExprAST>> elements;
  auto [first, first_result] = ParseExpr();
  if (first_result == NONCOMMITTED) {
    imm_error("Expected at least one element in array literal",
              left_bracket->get_location());
    return {nullptr, FAILED};
  }
  if (first_result != SUCCESS) {
    return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)), FAILED};
  }
  // Check for range syntax: [start...end]
  if (tokStream->peek()->tok_type == TokEllipsis) {
    tokStream->consume();
    auto [end_expr, end_result] = ParseExpr();
    if (end_result != SUCCESS) {
      imm_error("Expected expression after '...' in range",
                left_bracket->get_location());
      return {nullptr, FAILED};
    }
    auto _rb =
        expectOrError(TokRightBracket, "Expected ']' to close range expression",
                      left_bracket->get_location());
    if (!_rb)
      return {
          std::make_unique<RangeExprAST>(std::move(first), std::move(end_expr)),
          FAILED};
    return {
        std::make_unique<RangeExprAST>(std::move(first), std::move(end_expr)),
        SUCCESS};
  }

  elements.push_back(std::move(first));
  while (!tokStream->isEnd()) {
    auto comma = expect(TokComma);
    if (!comma)
      break;
    auto [elem, elem_result] = ParseExpr();
    if (elem_result == SUCCESS) {
      elements.push_back(std::move(elem));
      continue;
    }
    if (elem_result == NONCOMMITTED)
      imm_error("Expected expression after ',' in array literal",
                left_bracket->get_location());
    return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)), FAILED};
  }
  auto _rb =
      expectOrError(TokRightBracket, "Expected ']' to close array literal",
                    left_bracket->get_location());
  if (!_rb)
    return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)), FAILED};
  return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)), SUCCESS};
}

auto Parser::ParsePrimaryExpr() -> p<ExprAST> {
  // Unary prefix operators
  auto neg = [this]() {
    return ParseUnaryPrefixExpr<UnaryNegExprAST>(TokSUB, "-");
  };
  auto addr = [this]() {
    return ParseUnaryPrefixExpr<AddrOfExprAST>(TokAndLogical, "&");
  };
  // Builtin calls: keyword(expr)
  auto free_ = [this]() {
    return ParseBuiltinCallExpr<FreeExprAST>(TokFree, "free");
  };
  auto len = [this]() {
    return ParseBuiltinCallExpr<LenExprAST>(TokLen, "len");
  };
  auto dim = [this]() {
    return ParseBuiltinCallExpr<DimExprAST>(TokDim, "dim");
  };
  // Literal expressions
  auto num = [this]() -> p<ExprAST> {
    if (auto t = expect(TokNum))
      return {std::make_unique<NumberExprAST>(t), SUCCESS};
    return {nullptr, NONCOMMITTED};
  };
  auto str = [this]() -> p<ExprAST> {
    if (auto t = expect(TokStr))
      return {std::make_unique<StringExprAST>(t), SUCCESS};
    return {nullptr, NONCOMMITTED};
  };
  auto bool_ = [this]() -> p<ExprAST> {
    if (auto t = expect(TokTrue))
      return {std::make_unique<BoolExprAST>(true, t->get_location()), SUCCESS};
    if (auto t = expect(TokFalse))
      return {std::make_unique<BoolExprAST>(false, t->get_location()), SUCCESS};
    return {nullptr, NONCOMMITTED};
  };
  auto chr = [this]() -> p<ExprAST> {
    if (auto t = expect(TokChar)) {
      char v = t->lexeme.empty() ? '\0' : t->lexeme[0];
      return {std::make_unique<CharExprAST>(v, t->get_location()), SUCCESS};
    }
    return {nullptr, NONCOMMITTED};
  };

  auto result = tryParsers<ExprAST>(
      neg, &Parser::ParseDerefExpr, addr, &Parser::ParseAllocExpr, free_, len,
      dim, &Parser::ParseArrayLiteralExpr, &Parser::ParseIdentifierExpr,
      &Parser::ParseParenExpr, &Parser::ParseIfExpr, &Parser::ParseCaseExpr,
      &Parser::ParseWhileExpr, num, bool_, chr, str);
  if (!result.ok())
    return result;
  return parsePostfixOps(std::move(result.node));
}

auto Parser::parsePostfixOps(u<ExprAST> expr) -> p<ExprAST> {
  while (true) {
    if (auto lb = expect(TokenType::TokLeftBracket)) {
      auto [idx, idx_result] = ParseExpr();
      if (idx_result == NONCOMMITTED) {
        imm_error("Expected expression inside '[]'", lb->get_location());
        return {nullptr, FAILED};
      }
      if (idx_result != SUCCESS) {
        return {std::make_unique<IndexExprAST>(std::move(expr), std::move(idx)),
                FAILED};
      }
      auto _rb = expectOrError(TokRightBracket,
                               "Expected ']' to close index expression",
                               lb->get_location());
      if (!_rb)
        return {std::make_unique<IndexExprAST>(std::move(expr), std::move(idx)),
                FAILED};
      expr = std::make_unique<IndexExprAST>(std::move(expr), std::move(idx));
    } else if (auto dot = expect(TokenType::TokDot)) {
      auto field_tok = expectOrError(TokID, "Expected field name after '.'",
                                     dot->get_location());
      if (!field_tok)
        return {std::make_unique<FieldAccessExprAST>(std::move(expr), nullptr),
                FAILED};
      expr = std::make_unique<FieldAccessExprAST>(std::move(expr), field_tok);
    } else {
      break;
    }
  }
  return {std::move(expr), SUCCESS};
}

// Parse a full expression: primary + optional binary operators (precedence
// climbing).
auto Parser::ParseExpr() -> p<ExprAST> {
  auto [LHS, left_result] = ParsePrimaryExpr();
  if (left_result == NONCOMMITTED)
    return {std::move(LHS), NONCOMMITTED};
  if (left_result != SUCCESS) {
    return {std::move(LHS), FAILED};
  }

  auto [next, right_result] = ParseBinaryExpr(0, std::move(LHS));
  if (right_result == SUCCESS || right_result == NONCOMMITTED)
    return {std::move(next), SUCCESS};
  return {std::move(next), FAILED};
}

// Precedence-climbing parser for binary expressions.
// Pipe (|>) is desugared here: `x |> f(y)` becomes `f(x, y)`.
auto Parser::ParseBinaryExpr(int precedence, u<ExprAST> LHS) -> p<ExprAST> {
  while (!tokStream->isEnd()) {
    auto tok = tokStream->peek()->tok_type;
    int TokPrec = GetTokPrecedence(tok);

    if (TokPrec < precedence)
      return {std::move(LHS), SUCCESS};

    auto binOpToken = tokStream->consume(); // Consume the operator

    // Parse the RHS as a primary
    auto [RHS, right_result] = ParsePrimaryExpr();
    if (right_result != SUCCESS) {
      imm_error("Expected right-hand side expression after binary operator",
                binOpToken->get_location());
      return {nullptr, FAILED};
    }

    // Look ahead: if the next operator binds more tightly, parse it first
    auto nextTok = tokStream->peek()->tok_type;
    int NextPrec = GetTokPrecedence(nextTok);
    if (TokPrec < NextPrec) {
      auto nested = ParseBinaryExpr(TokPrec + 1, std::move(RHS));
      RHS = std::move(nested.node);
      right_result = nested.status;
      if (right_result != SUCCESS) {
        imm_error("Incomplete right-hand expression after binary operator",
                  binOpToken->get_location());
        return {nullptr, right_result};
      }
    }

    // Combine LHS and RHS
    if (binOpToken->tok_type == TokenType::TokPipe) {
      // x |> f      →  f(x)
      // x |> f(y,z) →  f(x,y,z)
      sammine_util::QualifiedName qn;
      sammine_util::Location qn_loc;
      std::vector<std::unique_ptr<ExprAST>> new_args;

      if (auto *call = llvm::dyn_cast<CallExprAST>(RHS.get())) {
        qn = call->functionName;
        qn_loc = call->get_location();
        for (auto &arg : call->arguments)
          new_args.push_back(std::move(arg));
      } else if (auto *var = llvm::dyn_cast<VariableExprAST>(RHS.get())) {
        qn = sammine_util::QualifiedName::local(var->variableName);
        qn_loc = var->get_location();
      } else {
        imm_error("Right-hand side of |> must be a function name or call",
                  binOpToken->get_location());
        return {nullptr, FAILED};
      }

      new_args.insert(new_args.begin(), std::move(LHS));
      LHS = std::make_unique<CallExprAST>(std::move(qn), qn_loc,
                                          std::move(new_args));
    } else {
      LHS = std::make_unique<BinaryExprAST>(binOpToken, std::move(LHS),
                                            std::move(RHS));
    }
  }

  return {std::move(LHS), SUCCESS};
}

auto Parser::ParseReturnStmt() -> p<ExprAST> {
  auto return_tok = expect(TokenType::TokReturn);
  if (!return_tok)
    return {std::make_unique<ReturnStmtAST>(nullptr, nullptr), NONCOMMITTED};
  auto [expr, result] = ParseExpr();
  if (result == FAILED)
    return {std::make_unique<ReturnStmtAST>(return_tok, std::move(expr)),
            FAILED};
  auto _semi = expectOrError(TokSemiColon, "Missing ';' after return statement",
                             return_tok->get_location());
  if (!_semi)
    return {std::make_unique<ReturnStmtAST>(return_tok, std::move(expr)),
            FAILED};
  if (result == NONCOMMITTED)
    return {std::make_unique<ReturnStmtAST>(return_tok,
                                            std::make_unique<UnitExprAST>()),
            SUCCESS};
  return {std::make_unique<ReturnStmtAST>(return_tok, std::move(expr)),
          SUCCESS};
}

auto Parser::ParseStructLiteralExpr(
    sammine_util::QualifiedName qn, Location qn_loc,
    std::vector<std::unique_ptr<TypeExprAST>> type_args) -> p<ExprAST> {
  auto lbrace = tokStream->consume(); // consume {
  std::vector<std::string> field_names;
  std::vector<std::unique_ptr<ExprAST>> field_values;
  bool had_error = false;

  while (tokStream->peek()->tok_type != TokRightCurly &&
         tokStream->peek()->tok_type != TokEOF) {
    auto field_id = expect(TokID);
    if (!field_id) {
      imm_error("Expected field name in struct literal",
                tokStream->currentLocation());
      had_error = true;
      break;
    }
    auto colon = expect(TokColon);
    if (!colon) {
      imm_error(
          fmt::format("Expected ':' after field name '{}'", field_id->lexeme),
          field_id->get_location());
      had_error = true;
      break;
    }
    auto [val, val_result] = ParseExpr();
    if (val_result == NONCOMMITTED) {
      imm_error(
          fmt::format("Expected expression for field '{}'", field_id->lexeme),
          colon->get_location());
      had_error = true;
      break;
    }
    if (val_result == FAILED) {
      field_names.push_back(field_id->lexeme);
      field_values.push_back(std::move(val));
      had_error = true;
      break;
    }
    field_names.push_back(field_id->lexeme);
    field_values.push_back(std::move(val));
    if (!expect(TokComma))
      break; // comma is optional before }
  }

  auto _rc =
      expectOrError(TokRightCurly, "Expected '}' to close struct literal",
                    lbrace->get_location());
  if (!_rc)
    return {std::make_unique<StructLiteralExprAST>(
                qn, qn_loc, std::move(field_names), std::move(field_values)),
            FAILED};
  auto sl = std::make_unique<StructLiteralExprAST>(
      qn, qn_loc, std::move(field_names), std::move(field_values));
  sl->explicit_type_args = std::move(type_args);
  return {std::move(sl), had_error ? FAILED : SUCCESS};
}

auto Parser::ParseIdentifierExpr() -> p<ExprAST> {
  auto id = expect(TokenType::TokID);
  if (!id)
    return {std::make_unique<CallExprAST>(nullptr), NONCOMMITTED};

  // Handle qualified names: alias::member or alias::enum::variant
  auto call_pqn = parseQualifiedNameTail(id);
  auto qn = call_pqn.qn;
  auto qn_loc = call_pqn.location;

  // Speculative parse: f<TypeExpr, ...>(args) or Name<Types>::member(args)
  auto explicit_type_args = parseExplicitTypeArgsTail(qn, qn_loc);

  // Check for struct literal: ID { field: value, ... }
  // Lookahead to distinguish struct literal from a block that follows
  // a variable expression (e.g. `if x { ... }`).
  // Struct literal requires `{ ID : ...` or `{ }`.
  auto is_struct_literal = [&]() -> bool {
    if (tokStream->peek()->tok_type != TokLeftCurly)
      return false;
    tokStream->consume(); // consume {
    if (tokStream->peek()->tok_type == TokRightCurly) {
      tokStream->rollback(1);
      return true; // empty struct: Name {}
    }
    if (tokStream->peek()->tok_type == TokID) {
      tokStream->consume(); // consume potential field name
      bool result = (tokStream->peek()->tok_type == TokColon);
      tokStream->rollback(2); // rollback field + {
      return result;
    }
    tokStream->rollback(1);
    return false;
  }();

  if (is_struct_literal)
    return ParseStructLiteralExpr(std::move(qn), qn_loc,
                                  std::move(explicit_type_args));

  auto [args, result] = ParseArguments();
  if (result == SUCCESS) {
    auto call = std::make_unique<CallExprAST>(qn, qn_loc, std::move(args));
    call->explicit_type_args = std::move(explicit_type_args);
    return {std::move(call), SUCCESS};
  }
  if (result == NONCOMMITTED) {
    // Qualified name without args (e.g. Color::Red) — preserve as CallExprAST
    // so the qualified name isn't lost
    if (qn.is_qualified()) {
      auto call = std::make_unique<CallExprAST>(qn, qn_loc);
      call->explicit_type_args = std::move(explicit_type_args);
      return {std::move(call), SUCCESS};
    }
    return {std::make_unique<VariableExprAST>(id), SUCCESS};
  }
  auto call = std::make_unique<CallExprAST>(qn, qn_loc, std::move(args));
  call->explicit_type_args = std::move(explicit_type_args);
  return {std::move(call), FAILED};
}

auto Parser::ParseIfExpr() -> p<ExprAST> {
  auto if_tok = expect(TokenType::TokIf);
  if (!if_tok)
    return {std::make_unique<IfExprAST>(nullptr, nullptr, nullptr),
            NONCOMMITTED};

  auto [cond, cond_result] = ParseExpr();
  if (cond_result != SUCCESS) {
    emit_if_uncommitted(cond_result, "Expected a condition after 'if'",
                        if_tok->get_location());
    return {std::make_unique<IfExprAST>(std::move(cond), nullptr, nullptr),
            FAILED};
  }

  auto [then_block, then_result] = ParseBlock();
  if (then_result != SUCCESS) {
    emit_if_uncommitted(then_result, "Expected a block after 'if' condition",
                        cond->get_location());
    return {std::make_unique<IfExprAST>(std::move(cond), std::move(then_block),
                                        nullptr),
            FAILED};
  }

  auto else_tok = expect(TokenType::TokElse);
  if (!else_tok) {
    return {std::make_unique<IfExprAST>(std::move(cond), std::move(then_block),
                                        std::make_unique<BlockAST>()),
            SUCCESS};
  }

  auto x = tokStream->peek();
  if (x->tok_type == TokenType::TokIf) {
    auto [else_if_expr, else_if_result] = ParseIfExpr();
    auto else_block = std::make_unique<BlockAST>();
    else_block->Statements.push_back(std::move(else_if_expr));
    return {std::make_unique<IfExprAST>(std::move(cond), std::move(then_block),
                                        std::move(else_block)),
            else_if_result};
  }

  auto [else_block, else_result] = ParseBlock();
  if (else_result != SUCCESS) {
    emit_if_uncommitted(else_result, "Expected a block after 'else'",
                        else_tok->get_location());
    return {std::make_unique<IfExprAST>(std::move(cond), std::move(then_block),
                                        std::move(else_block)),
            FAILED};
  }
  return {std::make_unique<IfExprAST>(std::move(cond), std::move(then_block),
                                      std::move(else_block)),
          SUCCESS};
}

auto Parser::parseCasePattern() -> std::optional<CasePattern> {
  CasePattern pattern;
  auto pat_tok = tokStream->peek();

  if (pat_tok->tok_type == TokID && pat_tok->lexeme == "_") {
    tokStream->consume();
    pattern.is_wildcard = true;
    pattern.variant_name = sammine_util::QualifiedName::local("_");
    pattern.location = pat_tok->get_location();
    return pattern;
  }

  // Literal patterns: numbers, booleans, characters, negative numbers
  using LK = CasePattern::LiteralKind;

  auto make_literal = [&](LK kind) -> std::optional<CasePattern> {
    tokStream->consume();
    pattern.is_literal = true;
    pattern.literal_kind = kind;
    pattern.literal_value = pat_tok->lexeme;
    pattern.location = pat_tok->get_location();
    return pattern;
  };

  if (pat_tok->tok_type == TokNum)
    return make_literal(LK::Integer);
  if (pat_tok->tok_type == TokTrue || pat_tok->tok_type == TokFalse)
    return make_literal(LK::Bool);
  if (pat_tok->tok_type == TokChar)
    return make_literal(LK::Char);

  if (pat_tok->tok_type == TokSUB) {
    auto minus_tok = tokStream->consume(); // consume -
    auto next = tokStream->peek();
    if (next->tok_type == TokNum) {
      auto num_tok = tokStream->consume(); // consume number
      pattern.is_literal = true;
      pattern.literal_kind = LK::Integer;
      pattern.literal_value = "-" + num_tok->lexeme;
      pattern.location = minus_tok->get_location() | num_tok->get_location();
      return pattern;
    }
    imm_error("Expected number after '-' in case pattern",
              minus_tok->get_location());
    return std::nullopt;
  }

  if (pat_tok->tok_type != TokID) {
    imm_error("Expected pattern in case arm", pat_tok->get_location());
    return std::nullopt;
  }

  auto prefix_tok = tokStream->consume();
  std::string enum_prefix = prefix_tok->lexeme;

  // Handle generic enum pattern: Option<i32>::Some(v)
  if (tokStream->peek()->tok_type == TokLESS) {
    tokStream->consume(); // consume <
    auto [type_args, ok] =
        comma_separated<TypeExprAST>([&] { return ParseTypeExpr(); }, *this);
    if (!ok || type_args.empty() || !consumeClosingAngleBracket()) {
      imm_error("Expected type arguments in generic enum pattern",
                prefix_tok->get_location());
      return std::nullopt;
    }
    // Build type_args string separately from enum_prefix
    std::string ta = "<";
    for (size_t i = 0; i < type_args.size(); i++) {
      if (i > 0)
        ta += ", ";
      ta += type_args[i]->to_string();
    }
    ta += ">";

    // Generic patterns always require :: and variant name
    auto dcolon =
        expectOrError(TokenType::TokDoubleColon,
                      fmt::format("Expected '::' after '{}' in case pattern",
                                  enum_prefix + ta),
                      prefix_tok->get_location());
    if (!dcolon)
      return std::nullopt;
    auto variant_tok =
        expectOrError(TokenType::TokID, "Expected variant name after '::'",
                      dcolon->get_location());
    if (!variant_tok)
      return std::nullopt;
    pattern.variant_name = sammine_util::QualifiedName::from_parts(
        std::vector<sammine_util::QualifiedName::Part>{
            {enum_prefix, ta}, {variant_tok->lexeme, ""}});
    pattern.location = prefix_tok->get_location() | variant_tok->get_location();
  } else {
    // Unified: handles unqualified, 2-segment (Enum::Variant),
    // and 3-segment (Module::Enum::Variant)
    auto pqn = parseQualifiedNameTail(prefix_tok, false);
    // For 3-segment module::enum::variant, resolve the module alias
    if (pqn.qn.depth() == 3) {
      auto &parts = pqn.qn.parts();
      auto it = alias_to_module.find(parts[0].name);
      bool unresolved = (it == alias_to_module.end());
      std::optional<std::string> module_alias;
      std::string resolved_mod;
      if (unresolved) {
        resolved_mod = parts[0].name;
      } else {
        module_alias = parts[0].name;
        resolved_mod = it->second;
      }
      pqn.qn = sammine_util::QualifiedName::from_parts(
          std::vector<sammine_util::QualifiedName::Part>{
              {resolved_mod, ""}, parts[1], parts[2]},
          unresolved, std::move(module_alias));
    }
    pattern.variant_name = pqn.qn;
    pattern.location = pqn.location;
  }

  // Optional payload bindings: Variant(a, b)
  if (auto lparen = expect(TokenType::TokLeftParen)) {
    while (!tokStream->isEnd() &&
           tokStream->peek()->tok_type != TokenType::TokRightParen) {
      auto binding_tok =
          expectOrError(TokenType::TokID, "Expected binding name in pattern",
                        lparen->get_location());
      if (!binding_tok)
        return std::nullopt;
      pattern.bindings.push_back(binding_tok->lexeme);
      pattern.location = pattern.location | binding_tok->get_location();
      if (!expect(TokenType::TokComma))
        break;
    }
    auto rparen = expectOrError(TokenType::TokRightParen,
                                "Expected ')' to close pattern bindings",
                                lparen->get_location());
    if (!rparen)
      return std::nullopt;
    pattern.location = pattern.location | rparen->get_location();
  }

  return pattern;
}

auto Parser::ParseCaseExpr() -> p<ExprAST> {
  auto case_tok = expect(TokenType::TokCase);
  if (!case_tok)
    return {nullptr, NONCOMMITTED};

  auto [scrutinee, scrut_result] = ParseExpr();
  if (scrut_result != SUCCESS) {
    emit_if_uncommitted(scrut_result, "Expected expression after 'case'",
                        case_tok->get_location());
    return {nullptr, FAILED};
  }

  auto left_curly = expectOrError(TokenType::TokLeftCurly,
                                  "Expected '{' after case scrutinee",
                                  scrutinee->get_location());
  if (!left_curly)
    return {nullptr, FAILED};

  std::vector<CaseArm> arms;
  while (!tokStream->isEnd() &&
         tokStream->peek()->tok_type != TokenType::TokRightCurly) {
    auto pattern = parseCasePattern();
    if (!pattern)
      return {nullptr, FAILED};

    auto arrow =
        expectOrError(TokenType::TokFatArrow,
                      "Expected '=>' after case pattern", pattern->location);
    if (!arrow)
      return {nullptr, FAILED};

    CaseArm arm;
    arm.pattern = std::move(*pattern);
    if (tokStream->peek()->tok_type == TokenType::TokLeftCurly) {
      auto [block, block_result] = ParseBlock();
      if (block_result != SUCCESS) {
        emit_if_uncommitted(block_result, "Expected block in case arm",
                            arrow->get_location());
        return {nullptr, FAILED};
      }
      arm.body = std::move(block);
    } else {
      auto [expr, expr_result] = ParseExpr();
      if (expr_result != SUCCESS) {
        emit_if_uncommitted(expr_result, "Expected expression in case arm",
                            arrow->get_location());
        return {nullptr, FAILED};
      }
      auto block = std::make_unique<BlockAST>();
      block->join_location(expr.get());
      block->Statements.push_back(std::move(expr));
      arm.body = std::move(block);
    }
    arms.push_back(std::move(arm));

    if (!expect(TokenType::TokComma))
      break;
  }

  auto right_curly = expectOrError(TokenType::TokRightCurly,
                                   "Expected '}' to close case expression",
                                   case_tok->get_location());
  if (!right_curly)
    return {nullptr, FAILED};

  if (arms.empty()) {
    imm_error("Case expression must have at least one arm",
              case_tok->get_location());
  }

  return {std::make_unique<CaseExprAST>(case_tok, std::move(scrutinee),
                                        std::move(arms)),
          SUCCESS};
}

auto Parser::ParseWhileExpr() -> p<ExprAST> {
  auto while_tok = expect(TokenType::TokWhile);
  if (!while_tok)
    return {nullptr, NONCOMMITTED};

  auto [cond, cond_result] = ParseExpr();
  if (cond_result != SUCCESS) {
    emit_if_uncommitted(cond_result, "Expected a condition after 'while'",
                        while_tok->get_location());
    return {std::make_unique<WhileExprAST>(std::move(cond), nullptr), FAILED};
  }

  auto [body, body_result] = ParseBlock();
  if (body_result != SUCCESS) {
    emit_if_uncommitted(body_result, "Expected a block after 'while' condition",
                        cond->get_location());
    return {std::make_unique<WhileExprAST>(std::move(cond), std::move(body)),
            FAILED};
  }

  auto result =
      std::make_unique<WhileExprAST>(std::move(cond), std::move(body));
  result->join_location(while_tok);
  return {std::move(result), SUCCESS};
}

auto Parser::ParsePrototype() -> p<PrototypeAST> {
  auto id = expect(TokID);
  if (!id)
    return {nullptr, NONCOMMITTED};

  auto proto_pqn = parseQualifiedNameTail(id, false);

  // Parse optional explicit type parameters: <T, U, ...>
  bool tp_error = false;
  auto type_params = parseTypeParams(id->get_location(), tp_error);
  if (tp_error)
    return {nullptr, FAILED};

  auto params_result = ParseParams();
  if (params_result.status != SUCCESS) {
    emit_if_uncommitted(
        params_result.status,
        fmt::format("Expected '(' for parameter list after '{}'", id->lexeme),
        id->get_location());
    return {nullptr, FAILED};
  }
  auto params = std::move(params_result.params);
  bool var_arg = params_result.is_var_arg;

  auto arrow = expect(TokArrow);
  if (!arrow) {
    auto proto = std::make_unique<PrototypeAST>(
        proto_pqn.qn, proto_pqn.location, std::move(params));
    proto->is_var_arg = var_arg;
    proto->type_params = std::move(type_params);
    return {std::move(proto), SUCCESS};
  }

  auto return_type_expr = ParseTypeExprTopLevel();
  bool has_return_type = return_type_expr != nullptr;
  if (!has_return_type)
    imm_error("Expected a return type after '->'", arrow->get_location());
  auto proto = std::make_unique<PrototypeAST>(proto_pqn.qn, proto_pqn.location,
                                              std::move(return_type_expr),
                                              std::move(params));
  proto->is_var_arg = var_arg;
  proto->type_params = std::move(type_params);
  return {std::move(proto), has_return_type ? SUCCESS : FAILED};
}

auto Parser::ParsePrototypeWithSemi(const std::string &semi_msg)
    -> p<PrototypeAST> {
  auto result = ParsePrototype();
  if (result.ok() && !expect(TokSemiColon))
    imm_error(semi_msg, tokStream->currentLocation());
  return result;
}

auto Parser::ParseBlock() -> p<BlockAST> {
  bool error = false;
  auto leftCurly = expect(TokLeftCurly);
  if (!leftCurly)
    return {nullptr, NONCOMMITTED};

  auto blockAST = std::make_unique<BlockAST>();
  while (!tokStream->isEnd()) {
    auto [a, a_result] = ParseExpr();
    if (a_result == SUCCESS) {
      auto tok = tokStream->peek();
      if (tok->tok_type == TokenType::TokSemiColon) {
        blockAST->Statements.push_back(std::move(a));
        tokStream->consume();
        continue;
      }
      if (tok->tok_type == TokenType::TokRightCurly) {
        blockAST->Statements.push_back(std::move(a));
        blockAST->Statements.back()->is_statement = false;
        break;
      }
      imm_error(fmt::format("Missing ';' after expression — found '{}' instead",
                            tok->lexeme),
                tok->get_location());
      blockAST->Statements.push_back(std::move(a));
      tokStream->exhaust_until(TokSemiColon);
      continue;
    }
    if (a_result != NONCOMMITTED) {
      if (a)
        blockAST->Statements.push_back(std::move(a));
      (void)expect(TokenType::TokSemiColon, /*exhausts=*/true);
      error = true;
      (void)expect(TokSemiColon);
      continue;
    }

    auto tryStatement = [&](auto parseResult) -> bool {
      auto &[node, result] = parseResult;
      if (result == NONCOMMITTED)
        return false;
      if (node)
        blockAST->Statements.push_back(std::move(node));
      if (result != SUCCESS)
        error = true;
      return true;
    };

    if (tryStatement(ParseVarDef()))
      continue;
    if (tryStatement(ParseReturnStmt()))
      continue;
    break;
  }

  auto rightCurly = expect(TokRightCurly, true, TokEOF,
                           "Expected '}' to close statement block");

  return {std::move(blockAST), (!rightCurly || error) ? FAILED : SUCCESS};
}

auto Parser::ParseParenExpr() -> p<ExprAST> {
  auto tok_left = expect(TokLeftParen);
  if (!tok_left) {
    return {nullptr, NONCOMMITTED};
  }

  auto [expr, result] = ParseExpr(); // Parse inner expression
  if (result == FAILED) {
    imm_error("Incomplete expression after '('", tok_left->get_location());
    return {std::unique_ptr<UnitExprAST>(), FAILED};
  }

  // Check for tuple: (expr, expr, ...)
  if (result == SUCCESS && tokStream->peek()->tok_type == TokComma) {
    std::vector<std::unique_ptr<ExprAST>> elements;
    elements.push_back(std::move(expr));
    while (expect(TokComma)) {
      auto [next_expr, next_result] = ParseExpr();
      if (next_result != SUCCESS) {
        imm_error("Expected expression after ',' in tuple",
                  tok_left->get_location());
        return {nullptr, FAILED};
      }
      elements.push_back(std::move(next_expr));
    }
    auto tok_right =
        expectOrError(TokRightParen, "Expected ')' to close tuple literal",
                      tok_left->get_location());
    if (!tok_right)
      return {nullptr, FAILED};
    auto tuple = std::make_unique<TupleLiteralExprAST>(std::move(elements));
    tuple->join_location(tok_left)->join_location(tok_right);
    return {std::move(tuple), SUCCESS};
  }

  auto tok_right = expectOrError(
      TokRightParen, "Expected ')' to match '(' for the expression",
      tok_left->get_location() | expr->get_location());
  if (!tok_right)
    return {std::move(expr), FAILED};

  if (result == NONCOMMITTED)
    return {std::make_unique<UnitExprAST>(tok_left, tok_right), SUCCESS};
  return {std::move(expr), SUCCESS};
}
// Parsing of parameters in a function call, we use leftParen and rightParen
// as appropriate stopping point
auto Parser::ParseParams() -> ParamsResult {
  auto leftParen = expect(TokLeftParen);
  if (leftParen == nullptr)
    return {{}, NONCOMMITTED, false};

  // Leading ellipsis: (...)
  if (tokStream->peek()->tok_type == TokEllipsis) {
    tokStream->consume();
    auto rightParen = expect(TokRightParen, true);
    if (!rightParen) {
      imm_error("Expected ')' after '...'", leftParen->get_location());
      return {{}, FAILED, true};
    }
    return {{}, SUCCESS, true};
  }

  bool var_arg = false;
  auto [vec, error] = comma_sep_recover<TypedVarAST>(
      *this, [&var_arg, this]() -> p<TypedVarAST> {
        // Trailing ellipsis after comma: (x: i32, ...)
        if (tokStream->peek()->tok_type == TokEllipsis) {
          tokStream->consume();
          var_arg = true;
          return {nullptr, NONCOMMITTED};
        }
        return ParseTypedVar();
      });

  auto rightParen = expect(TokRightParen, true);
  if (!rightParen) {
    imm_error("Expected ')' after parameters", leftParen->get_location());
    return {std::move(vec), FAILED, var_arg};
  }
  return {std::move(vec), error ? FAILED : SUCCESS, var_arg};
}

auto Parser::ParseArguments() -> ListResult<ExprAST> {
  auto leftParen = expect(TokLeftParen);
  if (!leftParen)
    return {{}, NONCOMMITTED};

  auto [vec, error] =
      comma_sep_recover<ExprAST>(*this, [&]() { return ParseExpr(); });

  auto rightParen = expect(TokRightParen, true);
  if (!rightParen) {
    imm_error("Expected ')' after arguments", leftParen->get_location());
    return {std::move(vec), FAILED};
  }
  return {std::move(vec), error ? FAILED : SUCCESS};
}

auto Parser::ParseTypeClassDecl() -> p<DefinitionAST> {
  auto kw = expect(TokTypeclass);
  if (!kw)
    return {nullptr, NONCOMMITTED};

  auto name_tok = expectOrError(
      TokID, "Expected type class name after 'typeclass'", kw->get_location());
  if (!name_tok)
    return {nullptr, FAILED};
  bool tp_error = false;
  auto type_params = parseTypeParams(name_tok->get_location(), tp_error);
  if (tp_error || type_params.empty()) {
    if (type_params.empty())
      imm_error("Expected '<' after type class name", name_tok->get_location());
    return {nullptr, FAILED};
  }
  auto _lc =
      expectOrError(TokLeftCurly, "Expected '{' after type class declaration",
                    kw->get_location());
  if (!_lc)
    return {nullptr, FAILED};

  auto methods = collect_until<PrototypeAST>(TokRightCurly, *this, [&]() {
    return ParsePrototypeWithSemi(
        "Missing ';' after typeclass method declaration");
  });
  auto _rc = expectOrError(TokRightCurly, "Expected '}' to close type class",
                           kw->get_location());
  if (!_rc)
    return {nullptr, FAILED};
  return {std::make_unique<TypeClassDeclAST>(
              kw, name_tok->lexeme, std::move(type_params), std::move(methods)),
          SUCCESS};
}

auto Parser::ParseTypeClassInstance() -> p<DefinitionAST> {
  auto kw = expect(TokInstance);
  if (!kw)
    return {nullptr, NONCOMMITTED};

  auto name_tok = expectOrError(
      TokID, "Expected type class name after 'instance'", kw->get_location());
  if (!name_tok)
    return {nullptr, FAILED};
  auto _lt = expectOrError(TokLESS, "Expected '<' after type class name",
                           name_tok->get_location());
  if (!_lt)
    return {nullptr, FAILED};

  auto [type_exprs, te_ok] =
      comma_separated<TypeExprAST>([&] { return ParseTypeExpr(); }, *this);
  if (!te_ok || type_exprs.empty()) {
    imm_error("Expected type in instance declaration", kw->get_location());
    return {nullptr, FAILED};
  }

  if (!consumeClosingAngleBracket()) {
    imm_error("Expected '>' to close instance type", kw->get_location());
    return {nullptr, FAILED};
  }
  auto _lc = expectOrError(TokLeftCurly,
                           "Expected '{' after typeclass instance declaration",
                           kw->get_location());
  if (!_lc)
    return {nullptr, FAILED};

  auto methods =
      collect_until<FuncDefAST>(TokRightCurly, *this, [&]() -> p<FuncDefAST> {
        auto [def, result] = ParseFuncDef();
        return {std::unique_ptr<FuncDefAST>(
                    static_cast<FuncDefAST *>(def.release())),
                result};
      });
  auto _rc = expectOrError(TokRightCurly, "Expected '}' to close instance",
                           kw->get_location());
  if (!_rc)
    return {nullptr, FAILED};
  return {std::make_unique<TypeClassInstanceAST>(
              kw, name_tok->lexeme, std::move(type_exprs), std::move(methods)),
          SUCCESS};
}

auto Parser::ParseKernelDef() -> p<DefinitionAST> {
  auto kernel_tok = expect(TokKernel);
  if (!kernel_tok)
    return {nullptr, NONCOMMITTED};

  auto [proto, proto_status] = ParsePrototype();
  if (proto_status != SUCCESS)
    return {nullptr, FAILED};

  // TODO: support generics for kernel functions
  if (proto->is_generic()) {
    imm_error("Generic kernel functions are not yet supported",
              proto->get_location());
    return {nullptr, FAILED};
  }

  auto lbrace =
      expectOrError(TokLeftCurly, "Expected '{' after kernel prototype",
                    kernel_tok->get_location());
  if (!lbrace)
    return {nullptr, FAILED};

  std::vector<std::unique_ptr<KernelExprAST>> exprs;
  while (!tokStream->isEnd() && tokStream->peek()->tok_type != TokRightCurly) {
    if (tokStream->peek()->tok_type == TokNum) {
      auto num_tok = tokStream->consume();
      exprs.push_back(std::make_unique<KernelNumberExprAST>(num_tok));
    } else if (tokStream->peek()->tok_type == TokID &&
               tokStream->peek()->lexeme == "map") {
      // map(array_name, (params) -> RetType { body })
      auto map_tok = tokStream->consume();

      auto _map_lp = expectOrError(TokLeftParen, "Expected '(' after 'map'",
                                   map_tok->get_location());
      if (!_map_lp)
        return {nullptr, FAILED};

      auto map_arr =
          expectOrError(TokID, "Expected array name as first argument to 'map'",
                        map_tok->get_location());
      if (!map_arr)
        return {nullptr, FAILED};
      std::string input_name = map_arr->lexeme;

      auto _map_comma =
          expectOrError(TokComma, "Expected ',' after array name in 'map'",
                        map_arr->get_location());
      if (!_map_comma)
        return {nullptr, FAILED};

      // Parse lambda: (params) -> RetType { body }
      auto params_result = ParseParams();
      if (params_result.status != SUCCESS) {
        imm_error("Expected lambda parameter list in 'map'",
                  map_tok->get_location());
        return {nullptr, FAILED};
      }

      auto _map_arrow = expectOrError(
          TokArrow, "Expected '->' after lambda parameters in 'map'",
          map_tok->get_location());
      if (!_map_arrow)
        return {nullptr, FAILED};

      auto ret_type = ParseTypeExprTopLevel();
      if (!ret_type) {
        imm_error("Expected return type after '->' in 'map' lambda",
                  map_tok->get_location());
        return {nullptr, FAILED};
      }

      auto lambda_proto = std::make_unique<PrototypeAST>(
          QualifiedName::local(""), map_tok->get_location(),
          std::move(ret_type), std::move(params_result.params));

      auto [block, block_status] = ParseBlock();
      if (block_status != SUCCESS) {
        imm_error("Expected block body for 'map' lambda",
                  map_tok->get_location());
        return {nullptr, FAILED};
      }

      auto _map_rp = expectOrError(TokRightParen, "Expected ')' to close 'map'",
                                   map_tok->get_location());
      if (!_map_rp)
        return {nullptr, FAILED};

      exprs.push_back(std::make_unique<KernelMapExprAST>(
          map_tok, std::move(input_name), std::move(lambda_proto),
          std::move(block)));
    } else if (tokStream->peek()->tok_type == TokID &&
               tokStream->peek()->lexeme == "reduce") {
      // reduce(array_name, op, identity_expr)
      auto reduce_tok = tokStream->consume();

      auto _red_lp = expectOrError(TokLeftParen, "Expected '(' after 'reduce'",
                                   reduce_tok->get_location());
      if (!_red_lp)
        return {nullptr, FAILED};

      auto red_arr = expectOrError(
          TokID, "Expected array name as first argument to 'reduce'",
          reduce_tok->get_location());
      if (!red_arr)
        return {nullptr, FAILED};
      std::string input_name = red_arr->lexeme;

      auto _red_comma1 =
          expectOrError(TokComma, "Expected ',' after array name in 'reduce'",
                        red_arr->get_location());
      if (!_red_comma1)
        return {nullptr, FAILED};

      // Consume operator token: +, -, *, /
      auto op_type = tokStream->peek()->tok_type;
      if (op_type != TokADD && op_type != TokSUB && op_type != TokMUL &&
          op_type != TokDIV) {
        imm_error("Expected operator (+, -, *, /) in 'reduce'",
                  tokStream->currentLocation());
        return {nullptr, FAILED};
      }
      auto op_tok = tokStream->consume();

      auto _red_comma2 =
          expectOrError(TokComma, "Expected ',' after operator in 'reduce'",
                        op_tok->get_location());
      if (!_red_comma2)
        return {nullptr, FAILED};

      auto [identity, identity_status] = ParseExpr();
      if (identity_status != SUCCESS) {
        imm_error("Expected identity value expression in 'reduce'",
                  op_tok->get_location());
        return {nullptr, FAILED};
      }

      auto _red_rp =
          expectOrError(TokRightParen, "Expected ')' to close 'reduce'",
                        reduce_tok->get_location());
      if (!_red_rp)
        return {nullptr, FAILED};

      exprs.push_back(std::make_unique<KernelReduceExprAST>(
          reduce_tok, std::move(input_name), op_tok, std::move(identity)));
    } else {
      imm_error("Expected expression inside kernel body",
                tokStream->currentLocation());
      tokStream->exhaust_until(TokRightCurly);
      break;
    }
  }

  auto rbrace =
      expectOrError(TokRightCurly, "Expected '}' to close kernel definition",
                    kernel_tok->get_location());
  if (!rbrace)
    return {nullptr, FAILED};

  auto body = std::make_unique<KernelBlockAST>(std::move(exprs));
  return {std::make_unique<KernelDefAST>(std::move(proto), std::move(body)),
          SUCCESS};
}

// Parse `::id` tail after an initial ID. Resolves module aliases (e.g. `m::add`
// where `m` was aliased via `import math as m`). Used for calls and struct
// literals.
auto Parser::parseQualifiedNameTail(std::shared_ptr<Token> first_tok,
                                    bool resolve_alias) -> ParsedQualifiedName {
  std::vector<std::string> parts = {first_tok->lexeme};
  Location loc = first_tok->get_location();

  while (tokStream->peek()->tok_type == TokDoubleColon) {
    tokStream->consume(); // consume ::
    if (tokStream->peek()->tok_type == TokID) {
      auto next = tokStream->consume();
      parts.push_back(next->lexeme);
      loc = loc | next->get_location();
    } else {
      // No TokID after ::, rollback the :: consumption
      tokStream->rollback(1);
      break;
    }
  }

  if (parts.size() == 1)
    return {sammine_util::QualifiedName::local(parts[0]), loc};

  bool unresolved = false;
  std::optional<std::string> module_alias;
  if (resolve_alias) {
    auto it = alias_to_module.find(parts[0]);
    if (it != alias_to_module.end()) {
      module_alias = parts[0]; // save original alias (e.g. "h")
      parts[0] = it->second;   // resolve to module name (e.g. "typed_helper")
    } else
      unresolved = true;
  }

  return {sammine_util::QualifiedName::from_parts(std::move(parts), unresolved,
                                                  std::move(module_alias)),
          loc};
}

// Speculatively parse `<Type, ...>` after a name. Uses rollback if the `<`
// turns out to be a comparison rather than generic type args. Disambiguated by
// what follows the closing `>`: `(` = call, `{` = struct literal, `::` =
// qualified member.
auto Parser::parseExplicitTypeArgsTail(sammine_util::QualifiedName &qn,
                                       sammine_util::Location &qn_loc)
    -> std::vector<std::unique_ptr<TypeExprAST>> {
  if (tokStream->peek()->tok_type != TokLESS)
    return {};

  tokStream->mark_rollback();
  tokStream->consume(); // consume '<'

  auto [parsed_types, speculative_ok_flag] =
      comma_separated<TypeExprAST>([&] { return ParseTypeExpr(); }, *this);
  bool speculative_ok = speculative_ok_flag && !parsed_types.empty() &&
                        consumeClosingAngleBracket();

  if (speculative_ok && tokStream->peek()->tok_type == TokDoubleColon) {
    // Name<Types>::member — generic enum variant or typeclass method
    tokStream->consume(); // consume ::
    auto member_id = expect(TokID);
    if (member_id) {
      // Build type_args separately, attach to existing parts
      std::string type_args_str = "<";
      for (size_t i = 0; i < parsed_types.size(); i++) {
        if (i > 0)
          type_args_str += ", ";
        type_args_str += parsed_types[i]->to_string();
      }
      type_args_str += ">";
      auto cur_parts = qn.parts();
      std::vector<sammine_util::QualifiedName::Part> new_parts(
          cur_parts.begin(), cur_parts.end());
      new_parts.back().type_args = type_args_str;
      new_parts.push_back({member_id->lexeme, ""});
      qn = sammine_util::QualifiedName::from_parts(std::move(new_parts));
      qn_loc = qn_loc | member_id->get_location();
      tokStream->commit_rollback();
      return std::move(parsed_types);
    }
    speculative_ok = false;
  } else if (speculative_ok && tokStream->peek()->tok_type == TokLeftCurly) {
    // Name<Types>{ — generic struct literal; keep qn unmangled,
    // type args are carried structurally on StructLiteralExprAST
    tokStream->commit_rollback();
    return std::move(parsed_types);
  } else if (speculative_ok && tokStream->peek()->tok_type == TokLeftParen) {
    // Name<Types>(args) — generic function call
    tokStream->commit_rollback();
    return std::move(parsed_types);
  } else {
    speculative_ok = false;
  }

  if (!speculative_ok)
    tokStream->rollback();
  return {};
}

// Try to consume a token of the given type. On failure: report error (if
// message given) and optionally exhaust tokens until `until` (error recovery —
// skips to the next semicolon or closing brace to resync the parser).
auto Parser::expect(TokenType tokType, bool exhausts, TokenType until,
                    const std::string &message) -> std::shared_ptr<Token> {
  auto currentToken = tokStream->peek();
  auto result = !tokStream->isEnd() && currentToken->tok_type == tokType;
  if (result) {
    return tokStream->consume();
  } else {
    // TODO: Add error reporting after this point.
    if (!message.empty() && tokStream->peek()->tok_type != TokEOF) {
      imm_error(message, tokStream->currentLocation());
    } else if (!message.empty()) {
      imm_error(message, currentToken->get_location());
    }
    if (exhausts) {
      last_exhaustible_loc = currentToken->get_location();
      tokStream->exhaust_until(until);
    }

    return nullptr;
  }
}

} // namespace sammine_lang
