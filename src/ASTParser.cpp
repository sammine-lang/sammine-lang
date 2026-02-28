
#include "parser/Parser.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "lex/Token.h"
#include <memory>
#include <utility>
//! \file Parser.cpp
//! \brief Implementation for Parser class, it takes in the token stream and
//! converts it into Parsing things suchs as programs, top-level (struct,
//! functions, global variables)

namespace sammine_lang {
using namespace AST;

/// RAII guard that calls cst_->finish_node() on destruction.
/// Call start() to open the node after the parse is committed.
/// If start() is never called (NONCOMMITTED path), destructor is a no-op.
struct CSTNodeGuard {
  cst::CSTBridge *cst_ = nullptr;
  bool active_ = false;
  CSTNodeGuard() = default;
  void start(cst::CSTBridge *cst, cst::CSTBridge::BridgeCheckpoint cp,
             cst::SyntaxKind kind) {
    cst_ = cst;
    if (cst_) {
      cst_->start_node_at(cp, kind);
      active_ = true;
    }
  }
  void start(cst::CSTBridge *cst, cst::SyntaxKind kind) {
    cst_ = cst;
    if (cst_) {
      cst_->start_node(kind);
      active_ = true;
    }
  }
  ~CSTNodeGuard() {
    if (active_)
      cst_->finish_node();
  }
  CSTNodeGuard(const CSTNodeGuard &) = delete;
  CSTNodeGuard &operator=(const CSTNodeGuard &) = delete;
};

//! \brief Holds the precedence of a binary operation
static std::map<TokenType, int> binopPrecedence = {
    {TokenType::TokPipe, 1},
    {TokenType::TokASSIGN, 2},
    {TokenType::TokOR, 3},
    {TokenType::TokORLogical, 4},
    {TokenType::TokAND, 5},
    {TokenType::TokXOR, 6},
    {TokenType::TokAndLogical, 7},
    {TokenType::TokLESS, 10},
    {TokenType::TokLessEqual, 10},
    {TokenType::TokGreaterEqual, 10},
    {TokenType::TokGREATER, 10},
    {TokenType::TokEQUAL, 10},
    {TokenType::TokNOTEqual, 10},
    {TokenType::TokSHL, 15},
    {TokenType::TokSHR, 15},
    {TokenType::TokADD, 20},
    {TokenType::TokSUB, 20},
    {TokenType::TokMUL, 40},
    {TokenType::TokDIV, 40},
    {TokenType::TokMOD, 40},
};

int GetTokPrecedence(TokenType tokType) {
  auto prec = binopPrecedence[tokType];
  return prec > 0 ? prec : -1;
}

auto Parser::Parse() -> u<ProgramAST> { return ParseProgram(); }

auto Parser::ParseImport() -> std::optional<AST::ImportDecl> {
  auto cp = cst_ ? cst_->checkpoint() : cst::CSTBridge::BridgeCheckpoint{};
  auto import_tok = expect(TokImport);
  if (!import_tok)
    return std::nullopt;

  CSTNodeGuard cst_guard;
  cst_guard.start(cst_, cp, cst::SyntaxKind::ImportDecl);

  REQUIRE(module_id, TokID, "Expected module name after 'import'",
          import_tok->get_location(), std::nullopt);

  AST::ImportDecl decl;
  decl.module_name = module_id->lexeme;

  // 'as <alias>' is optional; when omitted, externs are injected directly
  auto as_tok = expect(TokAs);
  if (as_tok) {
    REQUIRE(alias_id, TokID, "Expected alias after 'as' in import",
            as_tok->get_location(), std::nullopt);
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
    imm_error("Expected ';' after import statement", decl.location);
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
    this->imm_error("Expected a valid declaration",
                    tokStream->currentLocation());
  return programAST;
}

auto Parser::ParseDefinition() -> p<DefinitionAST> {
  // Can't use tryParsers for all — ParseStructDef/ParseEnumDef have optional
  // checkpoint params, which changes their function pointer type.
  auto result = tryParsers<DefinitionAST>(
      &Parser::ParseTypeClassDecl, &Parser::ParseTypeClassInstance);
  if (result.status != NONCOMMITTED) {
    if (result.failed() && result.node)
      result.node->pe = true;
    return result;
  }
  result = ParseStructDef();
  if (result.status != NONCOMMITTED) {
    if (result.failed() && result.node)
      result.node->pe = true;
    return result;
  }
  result = ParseEnumDef();
  if (result.status != NONCOMMITTED) {
    if (result.failed() && result.node)
      result.node->pe = true;
    return result;
  }
  result = ParseFuncDef();
  if (result.failed() && result.node)
    result.node->pe = true;
  return result;
}



auto Parser::ParseStructDef(
    std::optional<cst::CSTBridge::BridgeCheckpoint> precede_cp)
    -> p<DefinitionAST> {
  auto cp = precede_cp ? *precede_cp
                       : (cst_ ? cst_->checkpoint()
                               : cst::CSTBridge::BridgeCheckpoint{});
  auto struct_tok = expect(TokStruct);
  if (!struct_tok)
    return {nullptr, NONCOMMITTED};

  CSTNodeGuard cst_guard;
  cst_guard.start(cst_, cp, cst::SyntaxKind::StructDef);

  REQUIRE(id, TokID, "Expected an identifier after 'struct'",
          struct_tok->get_location(), {nullptr, FAILED});

  // Handle qualified name (mod::Struct) from .mni files
  std::string qualified_module;
  if (tokStream->peek()->tok_type == TokDoubleColon) {
    tokStream->consume(); // consume ::
    auto member = expect(TokID);
    if (member) {
      qualified_module = id->lexeme;
      id = member;
    }
  }

  REQUIRE(left_curly, TokLeftCurly,
          fmt::format("Expected '{{{{' after struct identifier {}", id->lexeme),
          struct_tok->get_location() | id->get_location(), {nullptr, FAILED});

  std::vector<std::unique_ptr<TypedVarAST>> struct_members;
  while (!tokStream->isEnd()) {
    auto [member, result] = ParseTypedVar();
    if (result == SUCCESS) {
      auto member_name = member->name;
      auto member_loc = member->get_location();
      struct_members.push_back(std::move(member));
      // TODO: allow last member without trailing comma
      REQUIRE(_comma, TokComma,
              fmt::format("Expected ',' after member {}", member_name),
              member_loc,
              {std::make_unique<StructDefAST>(id, std::move(struct_members)),
               FAILED});
      continue;
    }
    if (result != NONCOMMITTED) {
      return {std::make_unique<StructDefAST>(id, std::move(struct_members)),
              FAILED};
    }
    break;
  }

  auto right_curly = expect(TokRightCurly);
  if (!right_curly) {
    // pick the error location
    auto err_loc = struct_members.empty()
                       ? left_curly->get_location()
                       : struct_members.back()->get_location();

    // build the message
    auto msg =
        fmt::format("Expected '}}}}' to close struct '{}' after member '{}'",
                    struct_tok->lexeme, struct_members.back()->name);

    // In the case there's no members in the struct
    if (struct_members.empty()) {
      msg = fmt::format("Expected '}}}}' to close struct '{}'",
                        struct_tok->lexeme);
    }
    this->imm_error(msg, err_loc);

    return {std::make_unique<StructDefAST>(id, std::move(struct_members)),
            FAILED};
  }
  if (!expect(TokSemiColon))
    imm_error("Expected ';' after struct definition",
              right_curly->get_location());

  auto struct_def = std::make_unique<StructDefAST>(id, std::move(struct_members));
  if (!qualified_module.empty())
    struct_def->struct_name = sammine_util::QualifiedName::qualified(
        qualified_module, struct_def->struct_name.name);
  return {std::move(struct_def), SUCCESS};
}

// type Name = Variant1(Type) | Variant2 | Variant3(Type, Type);
// type Name = ExistingType;  (type alias)
auto Parser::ParseEnumDef(
    std::optional<cst::CSTBridge::BridgeCheckpoint> precede_cp)
    -> p<DefinitionAST> {
  auto cp = precede_cp ? *precede_cp
                       : (cst_ ? cst_->checkpoint()
                               : cst::CSTBridge::BridgeCheckpoint{});
  auto type_tok = expect(TokType);
  if (!type_tok)
    return {nullptr, NONCOMMITTED};

  CSTNodeGuard cst_guard; // opened below after disambiguation

  REQUIRE(id, TokID, "Expected an identifier after 'type'",
          type_tok->get_location(), {nullptr, FAILED});

  // Parse optional type parameters: type Option<T> = ...
  std::vector<std::string> enum_type_params;
  if (expect(TokLESS)) {
    auto first = expect(TokID);
    if (!first) {
      this->imm_error("Expected type parameter name after '<'",
                      id->get_location());
      return {nullptr, FAILED};
    }
    enum_type_params.push_back(first->lexeme);
    while (expect(TokComma)) {
      auto next = expect(TokID);
      if (!next) {
        this->imm_error("Expected type parameter name after ','",
                        id->get_location());
        return {nullptr, FAILED};
      }
      enum_type_params.push_back(next->lexeme);
    }
    if (!consumeClosingAngleBracket()) {
      this->imm_error("Expected '>' after type parameter list",
                      id->get_location());
      return {nullptr, FAILED};
    }
  }

  // Parse optional backing type: type Foo: u32 = ...
  std::optional<std::string> backing_type_name;
  if (expect(TokColon)) {
    auto bt_tok = expect(TokID);
    if (!bt_tok) {
      imm_error("Expected backing type name after ':'", id->get_location());
      return {nullptr, FAILED};
    }
    backing_type_name = bt_tok->lexeme;
  }

  REQUIRE(_eq, TokASSIGN,
          fmt::format("Expected '=' after type name '{}'", id->lexeme),
          id->get_location(), {nullptr, FAILED});

  // Disambiguate: enum vs type alias
  // If next tokens are ID followed by | or ID followed by ( → enum
  // Otherwise → type alias
  bool is_enum = false;
  if (tokStream->peek()->tok_type == TokID) {
    tokStream->suppress_on_consume(true);
    tokStream->mark_rollback();
    tokStream->consume(); // consume the ID
    auto next = tokStream->peek();
    if (next->tok_type == TokORLogical || next->tok_type == TokLeftParen) {
      // Enum: ID | ... or ID(...)
      is_enum = true;
    }
    tokStream->rollback();
    tokStream->suppress_on_consume(false);
  }

  // Type alias path
  if (!is_enum) {
    auto type_expr = ParseTypeExpr();
    if (!type_expr) {
      imm_error("Expected type expression in type alias",
                id->get_location());
      return {nullptr, FAILED};
    }
    if (!expect(TokSemiColon))
      imm_error("Expected ';' after type alias definition",
                id->get_location());
    auto alias = std::make_unique<TypeAliasDefAST>(id, std::move(type_expr));
    return {std::move(alias), SUCCESS};
  }

  // Enum path: parse variants separated by |
  std::vector<EnumVariantDef> variants;
  while (!tokStream->isEnd()) {
    auto variant_id = expect(TokID);
    if (!variant_id) {
      imm_error("Expected variant name in type definition",
                variants.empty() ? id->get_location()
                                 : variants.back().location);
      return {std::make_unique<EnumDefAST>(id, std::move(variants)), FAILED};
    }

    EnumVariantDef variant;
    variant.name = variant_id->lexeme;
    variant.location = variant_id->get_location();

    // Optional payload: (Type, Type, ...) or (integer_literal)
    if (expect(TokLeftParen)) {
      if (tokStream->peek()->tok_type != TokRightParen) {
        // Negative discriminant: Foo(-1) — not supported
        if (tokStream->peek()->tok_type == TokSUB) {
          imm_error(
              "Negative discriminant values are not supported in "
              "integer-backed enums",
              tokStream->peek()->get_location());
          return {std::make_unique<EnumDefAST>(id, std::move(variants)),
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
            return {std::make_unique<EnumDefAST>(id, std::move(variants)),
                    FAILED};
          }
          variant.discriminant_value = val;
        } else {
          // Type payload: Foo(i32, i32)
          auto first_type = ParseTypeExpr();
          if (!first_type) {
            imm_error(fmt::format("Expected type in payload of variant '{}'",
                                  variant.name),
                      variant.location);
            return {std::make_unique<EnumDefAST>(id, std::move(variants)), FAILED};
          }
          variant.payload_types.push_back(std::move(first_type));

          while (expect(TokComma)) {
            auto next_type = ParseTypeExpr();
            if (!next_type) {
              imm_error("Expected type after ',' in variant payload",
                        variant.location);
              return {std::make_unique<EnumDefAST>(id, std::move(variants)),
                      FAILED};
            }
            variant.payload_types.push_back(std::move(next_type));
          }
        }
      }
      REQUIRE(_rp, TokRightParen,
              fmt::format("Expected ')' to close payload of variant '{}'",
                          variant.name),
              variant.location,
              {std::make_unique<EnumDefAST>(id, std::move(variants)), FAILED});
    }

    variants.push_back(std::move(variant));

    // | means more variants, otherwise done
    if (!expect(TokORLogical))
      break;
  }

  if (!expect(TokSemiColon))
    imm_error("Expected ';' after type definition",
              variants.empty() ? id->get_location() : variants.back().location);

  auto enum_def = std::make_unique<EnumDefAST>(id, std::move(variants));
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

auto Parser::ParseFuncDef() -> p<DefinitionAST> {
  auto func_cp = cst_ ? cst_->checkpoint() : cst::CSTBridge::BridgeCheckpoint{};
  // Try optional 'export' prefix
  auto export_tok = expect(TokenType::TokExport);
  bool is_exported = export_tok != nullptr;

  // 'reuse' — parse as ExternAST
  // plain 'reuse' = module-private, 'export reuse' = re-exported
  if (auto reuse_tok = expect(TokenType::TokReuse)) {
    auto [prototype, result] = ParsePrototype();
    if (result == SUCCESS) {
      if (!expect(TokSemiColon))
        imm_error("Expected ';' after reuse declaration",
                  reuse_tok->get_location());
      auto node = std::make_unique<ExternAST>(std::move(prototype));
      node->is_exposed = is_exported;
      return {std::move(node), SUCCESS};
    }
    emit_if_uncommitted(result,
                          "Expected a function prototype after 'reuse'",
                          reuse_tok->get_location());
    (void)expect(TokenType::TokSemiColon);
    auto node = std::make_unique<ExternAST>(std::move(prototype));
    node->is_exposed = is_exported;
    return {std::move(node), FAILED};
  }

  // 'export struct' — delegate to struct parsing with export flag
  if (is_exported && tokStream->peek()->tok_type == TokStruct) {
    auto [node, result] = ParseStructDef(func_cp);
    if (node) {
      if (auto *sd = llvm::dyn_cast<StructDefAST>(node.get()))
        sd->is_exported = true;
    }
    return {std::move(node), result};
  }

  // 'export type' — delegate to type/enum parsing with export flag
  if (is_exported && tokStream->peek()->tok_type == TokType) {
    auto [node, result] = ParseEnumDef(func_cp);
    if (node) {
      if (auto *ed = llvm::dyn_cast<EnumDefAST>(node.get()))
        ed->is_exported = true;
      else if (auto *ta = llvm::dyn_cast<TypeAliasDefAST>(node.get()))
        ta->is_exported = true;
    }
    return {std::move(node), result};
  }

  // 'let' (possibly preceded by 'export')
  auto fn = expect(TokenType::TokLet);
  if (!fn) {
    if (is_exported) {
      imm_error("Expected 'let', 'struct', or 'type' after 'export'",
                export_tok->get_location());
      return {std::make_unique<FuncDefAST>(nullptr, nullptr), FAILED};
    }
    return {std::make_unique<FuncDefAST>(nullptr, nullptr), NONCOMMITTED};
  }

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
  auto node = std::make_unique<FuncDefAST>(std::move(prototype), std::move(block));
  node->is_exported = is_exported;
  return {std::move(node), SUCCESS};
}

//! Parsing implementation for a variable decl/def

//! Accepts a let, continue parsing inside and (enable error reporting if
//! possible). If a `let` is not found then return a nullptr.
auto Parser::ParseVarDef() -> p<ExprAST> {
  auto let = expect(TokenType::TokLet);
  if (!let)
    return {std::make_unique<VarDefAST>(nullptr, nullptr, nullptr),
            NONCOMMITTED};
  auto mut_tok = expect(TokenType::TokMUT);
  bool is_mutable = mut_tok != nullptr;

  // Check for destructuring: let (a, b) = expr;
  if (auto lparen = expect(TokenType::TokLeftParen)) {
    std::vector<std::unique_ptr<TypedVarAST>> vars;
    auto [first_var, first_result] = ParseTypedVar();
    if (first_result != SUCCESS) {
      this->imm_error("Expected variable name in destructuring pattern",
                      lparen->get_location());
      return {nullptr, FAILED};
    }
    vars.push_back(std::move(first_var));
    while (expect(TokenType::TokComma)) {
      auto [next_var, next_result] = ParseTypedVar();
      if (next_result != SUCCESS) {
        this->imm_error("Expected variable name after ',' in destructuring",
                        lparen->get_location());
        return {nullptr, FAILED};
      }
      vars.push_back(std::move(next_var));
    }
    REQUIRE(_rp, TokRightParen,
            "Expected ')' to close destructuring pattern",
            lparen->get_location(), {nullptr, FAILED});
    auto assign_tok = expect(TokenType::TokASSIGN, true, TokSemiColon);
    if (!assign_tok) {
      this->imm_error("Expected '=' after destructuring pattern",
                      lparen->get_location());
      return {nullptr, FAILED};
    }
    auto [expr, result] = ParseExpr();
    if (result != SUCCESS) {
      this->imm_error("Expected expression in destructuring variable definition",
                      let->get_location());
      return {nullptr, FAILED};
    }
    auto semicolon = expect(TokenType::TokSemiColon, true);
    if (!semicolon) {
      this->imm_error("Expected ';' after variable definition",
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
    this->imm_error("Expected '=' in variable definition",
                    typedVar->get_location());
    return {std::make_unique<VarDefAST>(let, std::move(typedVar), nullptr,
                                        is_mutable),
            FAILED};
  }
  auto [expr, result] = ParseExpr();
  if (result != SUCCESS) {
    this->imm_error(result == NONCOMMITTED
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
  this->imm_error("Expected ';' after variable definition",
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

auto Parser::ParseTypeExprTopLevel() -> std::unique_ptr<TypeExprAST> {
  return ParseTypeExpr();
}

auto Parser::ParseTypeExpr() -> std::unique_ptr<TypeExprAST> {
  if (auto tick = expect(TokenType::TokTick)) {
    if (auto ptr_tok = expect(TokenType::TokPtr)) {
      REQUIRE(_lt, TokLESS, "Expected '<' after 'ptr'",
              ptr_tok->get_location(), nullptr);
      auto inner = ParseTypeExpr();
      if (!inner) {
        this->imm_error("Expected type inside 'ptr<...>'",
                        ptr_tok->get_location());
        return nullptr;
      }
      if (!consumeClosingAngleBracket()) {
        this->imm_error("Expected '>' to close 'ptr<...>'",
                        ptr_tok->get_location());
        return nullptr;
      }
      auto result =
          std::make_unique<PointerTypeExprAST>(std::move(inner), /*is_linear=*/true);
      result->location = tick->get_location();
      return result;
    } else {
      this->imm_error(
          "Expected 'ptr' after tick (') for linear pointer type",
          tick->get_location());
      return nullptr;
    }
  }
  if (auto ptr_tok = expect(TokenType::TokPtr)) {
    REQUIRE(_lt, TokLESS, "Expected '<' after 'ptr'", ptr_tok->get_location(),
            nullptr);
    auto inner = ParseTypeExpr();
    if (!inner) {
      this->imm_error("Expected type inside 'ptr<...>'",
                      ptr_tok->get_location());
      return nullptr;
    }
    if (!consumeClosingAngleBracket()) {
      this->imm_error("Expected '>' to close 'ptr<...>'",
                      ptr_tok->get_location());
      return nullptr;
    }
    auto result = std::make_unique<PointerTypeExprAST>(std::move(inner));
    result->location = ptr_tok->get_location();
    return result;
  }
  if (auto lbracket = expect(TokenType::TokLeftBracket)) {
    auto elem = ParseTypeExpr();
    if (!elem) {
      this->imm_error("Expected element type in '[TYPE;SIZE]'",
                      lbracket->get_location());
      return nullptr;
    }
    REQUIRE(_semi, TokSemiColon,
            "Expected ';' after element type in '[TYPE;SIZE]'",
            lbracket->get_location(), nullptr);
    REQUIRE(size_tok, TokNum, "Expected integer size in '[TYPE;SIZE]'",
            lbracket->get_location(), nullptr);
    size_t arr_size = std::stoul(size_tok->lexeme);
    REQUIRE(_rb, TokRightBracket, "Expected ']' to close '[TYPE;SIZE]'",
            lbracket->get_location(), nullptr);
    auto result = std::make_unique<ArrayTypeExprAST>(std::move(elem), arr_size);
    result->location = lbracket->get_location();
    return result;
  }
  if (auto lparen = expect(TokenType::TokLeftParen)) {
    // Parse types inside parens: could be function type or tuple type
    std::vector<std::unique_ptr<TypeExprAST>> types;
    if (tokStream->peek()->tok_type != TokenType::TokRightParen) {
      auto first = ParseTypeExpr();
      if (!first) {
        this->imm_error("Expected type in parenthesized type expression",
                        lparen->get_location());
        return nullptr;
      }
      types.push_back(std::move(first));
      while (expect(TokenType::TokComma)) {
        auto param = ParseTypeExpr();
        if (!param) {
          this->imm_error("Expected type after ',' in type expression",
                          lparen->get_location());
          return nullptr;
        }
        types.push_back(std::move(param));
      }
    }
    REQUIRE(_rp, TokRightParen, "Expected ')' in type expression",
            lparen->get_location(), nullptr);
    // If '->' follows, it's a function type
    if (expect(TokenType::TokArrow)) {
      auto retType = ParseTypeExpr();
      if (!retType) {
        this->imm_error("Expected return type after '->' in function type",
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
    this->imm_error("Expected '->' for function type or ',' for tuple type",
                    lparen->get_location());
    return nullptr;
  }
  if (auto id = expect(TokenType::TokID)) {
    // Build the base name (simple or qualified)
    sammine_util::QualifiedName base_name =
        sammine_util::QualifiedName::local(id->lexeme);
    auto loc = id->get_location();

    if (tokStream->peek()->tok_type == TokDoubleColon) {
      tokStream->consume(); // consume ::
      REQUIRE(member_id, TokID,
              fmt::format("Expected type name after '{}::'", id->lexeme),
              id->get_location(), nullptr);
      base_name = resolveQualifiedName(id->lexeme, member_id->lexeme);
      loc = id->get_location() | member_id->get_location();
    }

    // Check for generic type args: Option<i32>, Result<i32, String>
    if (tokStream->peek()->tok_type == TokLESS) {
      tokStream->suppress_on_consume(true);
      tokStream->mark_rollback();
      tokStream->consume(); // consume <

      bool ok = true;
      std::vector<std::unique_ptr<TypeExprAST>> type_args;
      auto first_arg = ParseTypeExpr();
      if (!first_arg)
        ok = false;
      else {
        type_args.push_back(std::move(first_arg));
        while (ok && tokStream->peek()->tok_type == TokComma) {
          tokStream->consume(); // consume ,
          auto next_arg = ParseTypeExpr();
          if (!next_arg) {
            ok = false;
            break;
          }
          type_args.push_back(std::move(next_arg));
        }
        if (ok && !consumeClosingAngleBracket())
          ok = false;
      }

      if (!ok) {
        tokStream->rollback();
        tokStream->suppress_on_consume(false);
      } else {
        tokStream->suppress_on_consume(false);
        return std::make_unique<GenericTypeExprAST>(base_name,
                                                    std::move(type_args), loc);
      }
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
      this->imm_error("Expected identifier after 'mut'",
                      mut_tok->get_location());
      return {std::make_unique<TypedVarAST>(nullptr, nullptr), FAILED};
    }
    return {std::make_unique<TypedVarAST>(nullptr, nullptr), NONCOMMITTED};
  }

  auto colon = expect(TokenType::TokColon);
  if (!colon)
    return {std::make_unique<TypedVarAST>(name, is_mutable), SUCCESS};
  auto type_expr = ParseTypeExprTopLevel();
  if (!type_expr) {
    this->imm_error("Expected type name after token `:`",
                    colon->get_location());
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
    this->imm_error(fmt::format("Expected expression after '{}'", opStr),
                    tok->get_location());
    return {nullptr, FAILED};
  }
  return {std::make_unique<NodeT>(tok, std::move(operand)), FAILED};
}

auto Parser::ParseUnaryNegExpr() -> p<ExprAST> {
  return ParseUnaryPrefixExpr<UnaryNegExprAST>(TokSUB, "-");
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
    this->imm_error("Expected expression after '*'", star_tok->get_location());
    return {nullptr, FAILED};
  }
  return {std::make_unique<DerefExprAST>(star_tok, std::move(operand)), FAILED};
}

auto Parser::ParseAddrOfExpr() -> p<ExprAST> {
  return ParseUnaryPrefixExpr<AddrOfExprAST>(TokAndLogical, "&");
}

template <typename NodeT>
auto Parser::ParseBuiltinCallExpr(TokenType keyword, const std::string &name)
    -> p<ExprAST> {
  auto kw_tok = expect(keyword);
  if (!kw_tok)
    return {nullptr, NONCOMMITTED};
  REQUIRE(left_paren, TokLeftParen,
          fmt::format("Expected '(' after '{}'", name), kw_tok->get_location(),
          {nullptr, FAILED});
  auto [operand, result] = ParseExpr();
  if (result == NONCOMMITTED) {
    this->imm_error(fmt::format("Expected expression inside {}()", name),
                    kw_tok->get_location());
    return {nullptr, FAILED};
  }
  if (result != SUCCESS) {
    return {std::make_unique<NodeT>(kw_tok, std::move(operand)), FAILED};
  }
  REQUIRE(_rp, TokRightParen, fmt::format("Expected ')' to close {}()", name),
          kw_tok->get_location(),
          {std::make_unique<NodeT>(kw_tok, std::move(operand)), FAILED});
  return {std::make_unique<NodeT>(kw_tok, std::move(operand)), SUCCESS};
}

auto Parser::ParseAllocExpr() -> p<ExprAST> {
  auto kw_tok = expect(TokAlloc);
  if (!kw_tok)
    return {nullptr, NONCOMMITTED};

  // Parse <TypeExpr>
  REQUIRE(_lt, TokLESS, "Expected '<' after 'alloc'", kw_tok->get_location(),
          {nullptr, FAILED});
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
  REQUIRE(_lp, TokLeftParen, "Expected '(' after alloc<T>",
          kw_tok->get_location(), {nullptr, FAILED});
  auto [operand, result] = ParseExpr();
  if (result == NONCOMMITTED) {
    imm_error("Expected expression inside alloc<T>()", kw_tok->get_location());
    return {nullptr, FAILED};
  }
  if (result != SUCCESS)
    return {std::make_unique<AllocExprAST>(kw_tok, std::move(type_arg),
                                           std::move(operand)),
            FAILED};
  REQUIRE(_rp, TokRightParen, "Expected ')' to close alloc<T>()",
          kw_tok->get_location(),
          {std::make_unique<AllocExprAST>(kw_tok, std::move(type_arg),
                                          std::move(operand)),
           FAILED});
  return {std::make_unique<AllocExprAST>(kw_tok, std::move(type_arg),
                                         std::move(operand)),
          SUCCESS};
}

auto Parser::ParseFreeExpr() -> p<ExprAST> {
  return ParseBuiltinCallExpr<FreeExprAST>(TokFree, "free");
}

auto Parser::ParseArrayLiteralExpr() -> p<ExprAST> {
  auto left_bracket = expect(TokenType::TokLeftBracket);
  if (!left_bracket)
    return {nullptr, NONCOMMITTED};
  std::vector<u<ExprAST>> elements;
  auto [first, first_result] = ParseExpr();
  if (first_result == NONCOMMITTED) {
    this->imm_error("Expected at least one element in array literal",
                    left_bracket->get_location());
    return {nullptr, FAILED};
  }
  if (first_result != SUCCESS) {
    return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)), FAILED};
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
      this->imm_error("Expected expression after ',' in array literal",
                      left_bracket->get_location());
    return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)), FAILED};
  }
  REQUIRE(_rb, TokRightBracket, "Expected ']' to close array literal",
          left_bracket->get_location(),
          {std::make_unique<ArrayLiteralExprAST>(std::move(elements)), FAILED});
  return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)), SUCCESS};
}

auto Parser::ParseLenExpr() -> p<ExprAST> {
  return ParseBuiltinCallExpr<LenExprAST>(TokLen, "len");
}

auto Parser::ParsePrimaryExpr() -> p<ExprAST> {
  auto cp = cst_ ? cst_->checkpoint() : cst::CSTBridge::BridgeCheckpoint{};
  auto result = tryParsers<ExprAST>(
      &Parser::ParseUnaryNegExpr, &Parser::ParseDerefExpr,
      &Parser::ParseAddrOfExpr, &Parser::ParseAllocExpr, &Parser::ParseFreeExpr,
      &Parser::ParseLenExpr,
      &Parser::ParseArrayLiteralExpr, &Parser::ParseCallExpr,
      &Parser::ParseParenExpr, &Parser::ParseIfExpr, &Parser::ParseCaseExpr,
      &Parser::ParseWhileExpr,
      &Parser::ParseNumberExpr, &Parser::ParseBoolExpr,
      &Parser::ParseCharExpr, &Parser::ParseStringExpr);
  if (!result.ok())
    return result;
  return parsePostfixOps(std::move(result.node), cp);
}

auto Parser::parsePostfixOps(u<ExprAST> expr,
                             cst::CSTBridge::BridgeCheckpoint expr_cp)
    -> p<ExprAST> {
  while (true) {
    if (auto lb = expect(TokenType::TokLeftBracket)) {
      auto [idx, idx_result] = ParseExpr();
      if (idx_result == NONCOMMITTED) {
        this->imm_error("Expected expression inside '[]'", lb->get_location());
        return {nullptr, FAILED};
      }
      if (idx_result != SUCCESS) {
        return {std::make_unique<IndexExprAST>(std::move(expr), std::move(idx)),
                FAILED};
      }
      REQUIRE(_rb, TokRightBracket, "Expected ']' to close index expression",
              lb->get_location(),
              {std::make_unique<IndexExprAST>(std::move(expr), std::move(idx)),
               FAILED});
      expr = std::make_unique<IndexExprAST>(std::move(expr), std::move(idx));
    } else if (auto dot = expect(TokenType::TokDot)) {
      REQUIRE(field_tok, TokID, "Expected field name after '.'",
              dot->get_location(),
              {std::make_unique<FieldAccessExprAST>(std::move(expr), nullptr),
               FAILED});
      expr = std::make_unique<FieldAccessExprAST>(std::move(expr), field_tok);
    } else {
      break;
    }
  }
  return {std::move(expr), SUCCESS};
}

auto Parser::ParseExpr() -> p<ExprAST> {
  auto cp = cst_ ? cst_->checkpoint() : cst::CSTBridge::BridgeCheckpoint{};
  auto [LHS, left_result] = ParsePrimaryExpr();
  if (left_result == NONCOMMITTED)
    return {std::move(LHS), NONCOMMITTED};
  if (left_result != SUCCESS) {
    return {std::move(LHS), FAILED};
  }

  auto [next, right_result] = ParseBinaryExpr(0, std::move(LHS), cp);
  if (right_result == SUCCESS || right_result == NONCOMMITTED)
    return {std::move(next), SUCCESS};
  return {std::move(next), FAILED};
}

auto Parser::ParseBinaryExpr(int precedence, u<ExprAST> LHS,
                             cst::CSTBridge::BridgeCheckpoint lhs_cp)
    -> p<ExprAST> {
  while (!tokStream->isEnd()) {
    auto tok = tokStream->peek()->tok_type;
    int TokPrec = GetTokPrecedence(tok);

    if (TokPrec < precedence)
      return {std::move(LHS), SUCCESS};

    auto binOpToken = tokStream->consume(); // Consume the operator

    // Parse the RHS as a primary
    auto [RHS, right_result] = ParsePrimaryExpr();
    if (right_result != SUCCESS) {
      this->imm_error(
          "Expected right-hand side expression after binary operator",
          binOpToken->get_location());
      return {nullptr, FAILED};
    }

    // Look ahead: if the next operator binds more tightly, parse it first
    auto nextTok = tokStream->peek()->tok_type;
    int NextPrec = GetTokPrecedence(nextTok);
    if (TokPrec < NextPrec) {
      auto rhs_cp = cst_ ? cst_->checkpoint() : cst::CSTBridge::BridgeCheckpoint{};
      auto nested = ParseBinaryExpr(TokPrec + 1, std::move(RHS), rhs_cp);
      RHS = std::move(nested.node);
      right_result = nested.status;
      if (right_result != SUCCESS) {
        this->imm_error(
            "Incomplete right-hand expression after binary operator",
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
        this->imm_error("Right-hand side of |> must be a function name or call",
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

auto Parser::ParseReturnExpr() -> p<ExprAST> {
  auto return_tok = expect(TokenType::TokReturn);
  if (!return_tok)
    return {std::make_unique<ReturnExprAST>(nullptr, nullptr), NONCOMMITTED};
  auto [expr, result] = ParseExpr();
  if (result == FAILED)
    return {std::make_unique<ReturnExprAST>(return_tok, std::move(expr)),
            FAILED};
  REQUIRE(
      _semi, TokSemiColon, "Expected ';' after return statement",
      return_tok->get_location(),
      {std::make_unique<ReturnExprAST>(return_tok, std::move(expr)), FAILED});
  if (result == NONCOMMITTED)
    return {std::make_unique<ReturnExprAST>(return_tok,
                                            std::make_unique<UnitExprAST>()),
            SUCCESS};
  return {std::make_unique<ReturnExprAST>(return_tok, std::move(expr)),
          SUCCESS};
}

auto Parser::ParseStructLiteralExpr(sammine_util::QualifiedName qn,
                                    Location qn_loc,
                                    cst::CSTBridge::BridgeCheckpoint precede_cp)
    -> p<ExprAST> {
  auto lbrace = tokStream->consume(); // consume {
  std::vector<std::string> field_names;
  std::vector<std::unique_ptr<ExprAST>> field_values;
  bool had_error = false;

  while (tokStream->peek()->tok_type != TokRightCurly &&
         tokStream->peek()->tok_type != TokEOF) {
    auto field_id = expect(TokID);
    if (!field_id) {
      this->imm_error("Expected field name in struct literal",
                      tokStream->currentLocation());
      had_error = true;
      break;
    }
    auto colon = expect(TokColon);
    if (!colon) {
      this->imm_error(
          fmt::format("Expected ':' after field name '{}'", field_id->lexeme),
          field_id->get_location());
      had_error = true;
      break;
    }
    auto [val, val_result] = ParseExpr();
    if (val_result == NONCOMMITTED) {
      this->imm_error(
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

  REQUIRE(_rc, TokRightCurly, "Expected '}' to close struct literal",
          lbrace->get_location(),
          {std::make_unique<StructLiteralExprAST>(
               qn, qn_loc, std::move(field_names), std::move(field_values)),
           FAILED});
  return {std::make_unique<StructLiteralExprAST>(
              qn, qn_loc, std::move(field_names), std::move(field_values)),
          had_error ? FAILED : SUCCESS};
}

auto Parser::ParseCallExpr() -> p<ExprAST> {
  auto call_cp = cst_ ? cst_->checkpoint() : cst::CSTBridge::BridgeCheckpoint{};
  auto id = expect(TokenType::TokID);
  if (!id)
    return {std::make_unique<CallExprAST>(nullptr), NONCOMMITTED};

  // Handle qualified names: alias::member (e.g. m::add)
  sammine_util::QualifiedName qn =
      sammine_util::QualifiedName::local(id->lexeme);
  auto qn_loc = id->get_location();
  if (tokStream->peek()->tok_type == TokDoubleColon) {
    tokStream->consume(); // always consume the ::
    REQUIRE(member_id, TokID,
            fmt::format("Expected member name after '{}::'", id->lexeme),
            id->get_location(), {nullptr, FAILED});
    qn_loc = id->get_location() | member_id->get_location();
    qn = resolveQualifiedName(id->lexeme, member_id->lexeme);
  }

  // Speculative parse: f<TypeExpr, ...>(args) — generic call with explicit type args.
  // We try to parse <type_arg, ...> and check that a '(' follows.
  // If any step fails, we rollback and fall through to normal parsing.
  std::vector<std::unique_ptr<TypeExprAST>> explicit_type_args;
  if (tokStream->peek()->tok_type == TokLESS) {
    tokStream->suppress_on_consume(true);
    tokStream->mark_rollback();
    tokStream->consume(); // consume '<'

    bool speculative_ok = true;
    std::vector<std::unique_ptr<TypeExprAST>> parsed_types;

    auto first_type = ParseTypeExpr();
    if (!first_type)
      speculative_ok = false;
    else
      parsed_types.push_back(std::move(first_type));

    while (speculative_ok && tokStream->peek()->tok_type == TokComma) {
      tokStream->consume(); // consume ','
      auto next_type = ParseTypeExpr();
      if (!next_type) {
        speculative_ok = false;
        break;
      }
      parsed_types.push_back(std::move(next_type));
    }

    if (speculative_ok && !consumeClosingAngleBracket())
      speculative_ok = false;

    if (speculative_ok && tokStream->peek()->tok_type == TokDoubleColon) {
      // ID<Types>::Variant — generic enum constructor
      tokStream->consume(); // consume ::
      auto variant_id = expect(TokID);
      if (variant_id) {
        // Build mangled enum name: Option<i32>
        std::string mangled = qn.mangled() + "<";
        for (size_t i = 0; i < parsed_types.size(); i++) {
          if (i > 0) mangled += ", ";
          mangled += parsed_types[i]->to_string();
        }
        mangled += ">";
        qn = sammine_util::QualifiedName::qualified(mangled, variant_id->lexeme);
        qn_loc = id->get_location() | variant_id->get_location();
        explicit_type_args = std::move(parsed_types);
      } else {
        speculative_ok = false;
      }
    } else if (speculative_ok && tokStream->peek()->tok_type == TokLeftParen) {
      // ID<Types>(args) — generic function call
      explicit_type_args = std::move(parsed_types);
    } else {
      speculative_ok = false;
    }

    if (!speculative_ok) {
      tokStream->rollback();
    }
    tokStream->suppress_on_consume(false);
  }

  // Check for struct literal: ID { field: value, ... }
  // Lookahead to distinguish struct literal from a block that follows
  // a variable expression (e.g. `if x { ... }`).
  // Struct literal requires `{ ID : ...` or `{ }`.
  auto is_struct_literal = [&]() -> bool {
    if (tokStream->peek()->tok_type != TokLeftCurly)
      return false;
    tokStream->suppress_on_consume(true);
    tokStream->consume(); // consume {
    if (tokStream->peek()->tok_type == TokRightCurly) {
      tokStream->rollback(1);
      tokStream->suppress_on_consume(false);
      return true; // empty struct: Name {}
    }
    if (tokStream->peek()->tok_type == TokID) {
      tokStream->consume(); // consume potential field name
      bool result = (tokStream->peek()->tok_type == TokColon);
      tokStream->rollback(2); // rollback field + {
      tokStream->suppress_on_consume(false);
      return result;
    }
    tokStream->rollback(1);
    tokStream->suppress_on_consume(false);
    return false;
  }();

  if (is_struct_literal)
    return ParseStructLiteralExpr(std::move(qn), qn_loc, call_cp);

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

  auto left_curly = expect(TokenType::TokLeftCurly);
  if (!left_curly) {
    imm_error("Expected '{' after case scrutinee",
              scrutinee->get_location());
    return {nullptr, FAILED};
  }

  std::vector<CaseArm> arms;
  while (!tokStream->isEnd() &&
         tokStream->peek()->tok_type != TokenType::TokRightCurly) {
    CasePattern pattern;
    auto pat_tok = tokStream->peek();

    if (pat_tok->tok_type == TokID && pat_tok->lexeme == "_") {
      tokStream->consume();
      pattern.is_wildcard = true;
      pattern.variant_name = sammine_util::QualifiedName::local("_");
      pattern.location = pat_tok->get_location();
    } else if (pat_tok->tok_type == TokID) {
      auto prefix_tok = tokStream->consume();
      std::string enum_prefix = prefix_tok->lexeme;

      // Handle generic enum pattern: Option<i32>::Some(v)
      if (tokStream->peek()->tok_type == TokLESS) {
        tokStream->consume(); // consume <
        std::vector<std::unique_ptr<TypeExprAST>> type_args;
        bool ok = true;
        auto first_arg = ParseTypeExpr();
        if (!first_arg)
          ok = false;
        else {
          type_args.push_back(std::move(first_arg));
          while (ok && tokStream->peek()->tok_type == TokComma) {
            tokStream->consume();
            auto next_arg = ParseTypeExpr();
            if (!next_arg) { ok = false; break; }
            type_args.push_back(std::move(next_arg));
          }
          if (ok && !consumeClosingAngleBracket())
            ok = false;
        }
        if (!ok) {
          imm_error("Expected type arguments in generic enum pattern",
                    prefix_tok->get_location());
          return {nullptr, FAILED};
        }
        // Build mangled name: Option<i32>
        enum_prefix += "<";
        for (size_t i = 0; i < type_args.size(); i++) {
          if (i > 0) enum_prefix += ", ";
          enum_prefix += type_args[i]->to_string();
        }
        enum_prefix += ">";

        // Generic patterns always require :: and variant name
        auto dcolon = expect(TokenType::TokDoubleColon);
        if (!dcolon) {
          imm_error(
              fmt::format("Expected '::' after '{}' in case pattern",
                          enum_prefix),
              prefix_tok->get_location());
          return {nullptr, FAILED};
        }
        auto variant_tok = expect(TokenType::TokID);
        if (!variant_tok) {
          imm_error("Expected variant name after '::'",
                    dcolon->get_location());
          return {nullptr, FAILED};
        }
        pattern.variant_name =
            sammine_util::QualifiedName::qualified(enum_prefix, variant_tok->lexeme);
        pattern.location =
            prefix_tok->get_location() | variant_tok->get_location();

      } else if (tokStream->peek()->tok_type == TokDoubleColon) {
        // Qualified non-generic: Enum::Variant
        auto dcolon = tokStream->consume();
        auto variant_tok = expect(TokenType::TokID);
        if (!variant_tok) {
          imm_error("Expected variant name after '::'",
                    dcolon->get_location());
          return {nullptr, FAILED};
        }
        pattern.variant_name =
            sammine_util::QualifiedName::qualified(enum_prefix, variant_tok->lexeme);
        pattern.location =
            prefix_tok->get_location() | variant_tok->get_location();

      } else {
        // Unqualified: just the variant name (e.g., Some, None, Red)
        pattern.variant_name =
            sammine_util::QualifiedName::local(prefix_tok->lexeme);
        pattern.location = prefix_tok->get_location();
      }

      if (auto lparen = expect(TokenType::TokLeftParen)) {
        while (!tokStream->isEnd() &&
               tokStream->peek()->tok_type != TokenType::TokRightParen) {
          auto binding_tok = expect(TokenType::TokID);
          if (!binding_tok) {
            imm_error("Expected binding name in pattern",
                      lparen->get_location());
            return {nullptr, FAILED};
          }
          pattern.bindings.push_back(binding_tok->lexeme);
          pattern.location = pattern.location | binding_tok->get_location();
          if (!expect(TokenType::TokComma))
            break;
        }
        auto rparen = expect(TokenType::TokRightParen);
        if (!rparen) {
          imm_error("Expected ')' to close pattern bindings",
                    lparen->get_location());
          return {nullptr, FAILED};
        }
        pattern.location = pattern.location | rparen->get_location();
      }
    } else {
      imm_error("Expected pattern in case arm", pat_tok->get_location());
      return {nullptr, FAILED};
    }

    auto arrow = expect(TokenType::TokFatArrow);
    if (!arrow) {
      imm_error("Expected '=>' after case pattern", pattern.location);
      return {nullptr, FAILED};
    }

    CaseArm arm;
    arm.pattern = std::move(pattern);
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

  auto right_curly = expect(TokenType::TokRightCurly);
  if (!right_curly) {
    imm_error("Expected '}' to close case expression",
              case_tok->get_location());
    return {nullptr, FAILED};
  }

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

  auto result = std::make_unique<WhileExprAST>(std::move(cond), std::move(body));
  result->join_location(while_tok);
  return {std::move(result), SUCCESS};
}

auto Parser::ParseStringExpr() -> p<ExprAST> {
  if (auto tok_str = expect(TokStr)) {
    return {std::make_unique<StringExprAST>(tok_str), SUCCESS};
  }
  return {nullptr, NONCOMMITTED};
}

auto Parser::ParseNumberExpr() -> p<ExprAST> {
  if (auto numberToken = expect(TokenType::TokNum))
    return {std::make_unique<NumberExprAST>(numberToken), SUCCESS};
  return {nullptr, NONCOMMITTED};
}

auto Parser::ParseBoolExpr() -> p<ExprAST> {
  if (auto true_tok = expect(TokenType::TokTrue)) {
    return {std::make_unique<BoolExprAST>(true, true_tok->get_location()),
            SUCCESS};
  }
  if (auto false_tok = expect(TokenType::TokFalse)) {
    return {std::make_unique<BoolExprAST>(false, false_tok->get_location()),
            SUCCESS};
  }

  return {nullptr, NONCOMMITTED};
}

auto Parser::ParseCharExpr() -> p<ExprAST> {
  if (auto tok = expect(TokenType::TokChar)) {
    char value = tok->lexeme.empty() ? '\0' : tok->lexeme[0];
    return {std::make_unique<CharExprAST>(value, tok->get_location()), SUCCESS};
  }
  return {nullptr, NONCOMMITTED};
}

auto Parser::ParseVariableExpr() -> p<ExprAST> {
  if (auto name = expect(TokenType::TokID)) {
    return {std::make_unique<VariableExprAST>(name), SUCCESS};
  }
  return {nullptr, NONCOMMITTED};
}

auto Parser::ParsePrototype() -> p<PrototypeAST> {
  auto id = expect(TokID);
  if (!id)
    return {nullptr, NONCOMMITTED};

  // Handle qualified name (mod::func) from .mni files
  std::string qualified_module;
  if (tokStream->peek()->tok_type == TokDoubleColon) {
    tokStream->consume(); // consume ::
    auto member = expect(TokID);
    if (member) {
      qualified_module = id->lexeme;
      id = member; // id now holds the function name token
    }
  }

  // Parse optional explicit type parameters: <T, U, ...>
  std::vector<std::string> type_params;
  if (expect(TokLESS)) {
    auto first = expect(TokID);
    if (!first) {
      this->imm_error("Expected type parameter name after '<'",
                      id->get_location());
      return {nullptr, FAILED};
    }
    type_params.push_back(first->lexeme);
    while (expect(TokComma)) {
      auto next = expect(TokID);
      if (!next) {
        this->imm_error("Expected type parameter name after ','",
                        id->get_location());
        return {nullptr, FAILED};
      }
      type_params.push_back(next->lexeme);
    }
    if (!consumeClosingAngleBracket()) {
      this->imm_error("Expected '>' after type parameter list",
                      id->get_location());
      return {nullptr, FAILED};
    }
  }

  auto [params, result] = ParseParams();
  if (result != SUCCESS) {
    emit_if_uncommitted(
        result,
        fmt::format("Expected '(' for parameter list after '{}'", id->lexeme),
        id->get_location());
    return {nullptr, FAILED};
  }
  bool var_arg = parsed_var_arg;

  auto arrow = expect(TokArrow);
  if (!arrow) {
    auto proto = std::make_unique<PrototypeAST>(id, std::move(params));
    proto->is_var_arg = var_arg;
    proto->type_params = std::move(type_params);
    if (!qualified_module.empty())
      proto->functionName = sammine_util::QualifiedName::qualified(
          qualified_module, proto->functionName.name);
    return {std::move(proto), SUCCESS};
  }

  auto return_type_expr = ParseTypeExprTopLevel();
  bool has_return_type = return_type_expr != nullptr;
  if (!has_return_type)
    this->imm_error("Expected a return type after '->'", arrow->get_location());
  auto proto = std::make_unique<PrototypeAST>(id, std::move(return_type_expr),
                                              std::move(params));
  proto->is_var_arg = var_arg;
  proto->type_params = std::move(type_params);
  if (!qualified_module.empty())
    proto->functionName = sammine_util::QualifiedName::qualified(
        qualified_module, proto->functionName.name);
  return {std::move(proto), has_return_type ? SUCCESS : FAILED};
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
      auto tok = this->tokStream->peek();
      if (tok->tok_type == TokenType::TokSemiColon) {
        blockAST->Statements.push_back(std::move(a));
        this->tokStream->consume();
        continue;
      }
      if (tok->tok_type == TokenType::TokRightCurly) {
        blockAST->Statements.push_back(std::move(a));
        blockAST->Statements.back()->is_statement = false;
        break;
      }
      this->imm_error(
          fmt::format("Expected ';' after expression statement, found '{}'",
                      tok->lexeme),
          tok->get_location());
      blockAST->Statements.push_back(std::move(a));
      this->tokStream->exhaust_until(TokSemiColon);
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
    if (tryStatement(ParseReturnExpr()))
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
    this->imm_error("Incomplete expression after '('",
                    tok_left->get_location());
    return {std::unique_ptr<UnitExprAST>(), FAILED};
  }

  // Check for tuple: (expr, expr, ...)
  if (result == SUCCESS && tokStream->peek()->tok_type == TokComma) {
    std::vector<std::unique_ptr<ExprAST>> elements;
    elements.push_back(std::move(expr));
    while (expect(TokComma)) {
      auto [next_expr, next_result] = ParseExpr();
      if (next_result != SUCCESS) {
        this->imm_error("Expected expression after ',' in tuple",
                        tok_left->get_location());
        return {nullptr, FAILED};
      }
      elements.push_back(std::move(next_expr));
    }
    REQUIRE(tok_right, TokRightParen,
            "Expected ')' to close tuple literal",
            tok_left->get_location(),
            {nullptr, FAILED});
    auto tuple = std::make_unique<TupleLiteralExprAST>(std::move(elements));
    tuple->join_location(tok_left)->join_location(tok_right);
    return {std::move(tuple), SUCCESS};
  }

  REQUIRE(tok_right, TokRightParen,
          "Expected ')' to match '(' for the expression",
          tok_left->get_location() | expr->get_location(),
          {std::move(expr), FAILED});

  if (result == NONCOMMITTED)
    return {std::make_unique<UnitExprAST>(tok_left, tok_right), SUCCESS};
  return {std::move(expr), SUCCESS};
}
// Parsing of parameters in a function call, we use leftParen and rightParen
// as appropriate stopping point
auto Parser::ParseParams() -> ListResult<TypedVarAST> {
  auto leftParen = expect(TokLeftParen);
  if (leftParen == nullptr)
    return {std::vector<u<TypedVarAST>>(), NONCOMMITTED};

  // COMMITMENT POINT
  bool error = false;
  parsed_var_arg = false;

  // Check for leading ellipsis: (...)
  if (tokStream->peek()->tok_type == TokEllipsis) {
    tokStream->consume();
    parsed_var_arg = true;
    auto rightParen = expect(TokRightParen, true);
    if (!rightParen) {
      this->imm_error("Expected ')' after '...'", leftParen->get_location());
      return {std::vector<u<TypedVarAST>>(), FAILED};
    }
    return {std::vector<u<TypedVarAST>>(), SUCCESS};
  }

  auto [tv, tv_result] = ParseTypedVar();

  std::vector<u<TypedVarAST>> vec;
  if (tv_result == NONCOMMITTED) {
    auto rightParen = expect(TokRightParen, true);
    if (!rightParen) {
      this->imm_error("Expected ')' after parameters",
                      leftParen->get_location());
      return {std::move(vec), FAILED};
    }
    return {std::move(vec), SUCCESS};
  }
  vec.push_back(std::move(tv));
  while (!tokStream->isEnd()) {
    auto comma = expect(TokComma);
    if (comma == nullptr)
      break;

    // Check for trailing ellipsis: (x : i32, ...)
    if (tokStream->peek()->tok_type == TokEllipsis) {
      tokStream->consume();
      parsed_var_arg = true;
      break;
    }

    // Report error if we find comma but cannot find typeVar
    auto [nvt, nvt_result] = ParseTypedVar();
    if (nvt_result == SUCCESS) {
      vec.push_back(std::move(nvt));
      continue;
    }
    if (nvt_result == NONCOMMITTED) {
      auto rightParen = expect(TokRightParen, true);
      if (!rightParen) {
        this->imm_error("Expected ')' after parameters",
                        leftParen->get_location());
        return {std::move(vec), FAILED};
      }
      return {std::move(vec), error ? FAILED : SUCCESS};
    }
    vec.push_back(std::move(nvt));
    (void)expect(TokComma, true, TokComma);
    error = true;
  }
  auto rightParen = expect(TokRightParen, true);
  if (!rightParen) {
    this->imm_error("Expected ')' after parameters", leftParen->get_location());
    return {std::move(vec), FAILED};
  }
  return {std::move(vec), error ? FAILED : SUCCESS};
}

auto Parser::ParseArguments() -> ListResult<ExprAST> {
  auto leftParen = expect(TokLeftParen);
  bool error = false;
  if (!leftParen)
    return {std::vector<u<ExprAST>>(), NONCOMMITTED};

  std::vector<u<ExprAST>> vec;

  auto [first_expr, first_result] = ParseExpr();
  if (first_result == SUCCESS) {
    vec.push_back(std::move(first_expr));
  } else if (first_result != NONCOMMITTED) {
    vec.push_back(std::move(first_expr));
    (void)expect(TokComma, true, TokComma);
    error = true;
  }
  while (!tokStream->isEnd()) {
    auto comma = expect(TokComma);
    if (comma == nullptr)
      break;

    auto [expr, expr_result] = ParseExpr();
    if (expr_result == SUCCESS) {
      vec.push_back(std::move(expr));
      continue;
    }
    if (expr_result == NONCOMMITTED) {
      auto rightParen = expect(TokRightParen, true);
      if (!rightParen) {
        this->imm_error("Expected ')' after arguments",
                        leftParen->get_location());
        return {std::move(vec), FAILED};
      }
      return {std::move(vec), error ? FAILED : SUCCESS};
    }
    vec.push_back(std::move(expr));
    (void)expect(TokComma, true, TokComma);
    error = true;
  }
  auto rightParen = expect(TokRightParen, true);
  if (!rightParen) {
    this->imm_error("Expected ')' after arguments", leftParen->get_location());
    return {std::move(vec), FAILED};
  }
  return {std::move(vec), error ? FAILED : SUCCESS};
}

auto Parser::ParseTypeClassDecl() -> p<DefinitionAST> {
  auto kw = expect(TokTypeclass);
  if (!kw)
    return {nullptr, NONCOMMITTED};

  REQUIRE(name_tok, TokID, "Expected type class name after 'typeclass'",
          kw->get_location(), {nullptr, FAILED});
  REQUIRE(_lt, TokLESS, "Expected '<' after type class name",
          name_tok->get_location(), {nullptr, FAILED});
  REQUIRE(param_tok, TokID, "Expected type parameter",
          name_tok->get_location(), {nullptr, FAILED});
  if (!consumeClosingAngleBracket()) {
    imm_error("Expected '>' to close type class type parameter",
              param_tok->get_location());
    return {nullptr, FAILED};
  }
  REQUIRE(_lc, TokLeftCurly, "Expected '{' after type class declaration",
          kw->get_location(), {nullptr, FAILED});

  std::vector<std::unique_ptr<PrototypeAST>> methods;
  while (!tokStream->isEnd() && tokStream->peek()->tok_type != TokRightCurly) {
    auto [proto, result] = ParsePrototype();
    if (result != SUCCESS)
      break;
    (void)expect(TokSemiColon); // optional semicolon
    methods.push_back(std::move(proto));
  }
  REQUIRE(_rc, TokRightCurly, "Expected '}' to close type class",
          kw->get_location(), {nullptr, FAILED});
  return {std::make_unique<TypeClassDeclAST>(kw, name_tok->lexeme,
                                             param_tok->lexeme,
                                             std::move(methods)),
          SUCCESS};
}

auto Parser::ParseTypeClassInstance() -> p<DefinitionAST> {
  auto kw = expect(TokInstance);
  if (!kw)
    return {nullptr, NONCOMMITTED};

  REQUIRE(name_tok, TokID, "Expected type class name after 'instance'",
          kw->get_location(), {nullptr, FAILED});
  REQUIRE(_lt, TokLESS, "Expected '<' after type class name",
          name_tok->get_location(), {nullptr, FAILED});
  auto type_expr = ParseTypeExpr();
  if (!type_expr) {
    imm_error("Expected type in instance declaration", kw->get_location());
    return {nullptr, FAILED};
  }
  if (!consumeClosingAngleBracket()) {
    imm_error("Expected '>' to close instance type", kw->get_location());
    return {nullptr, FAILED};
  }
  REQUIRE(_lc, TokLeftCurly, "Expected '{' after instance declaration",
          kw->get_location(), {nullptr, FAILED});

  std::vector<std::unique_ptr<FuncDefAST>> methods;
  while (!tokStream->isEnd() && tokStream->peek()->tok_type != TokRightCurly) {
    auto [def, result] = ParseFuncDef();
    if (result != SUCCESS)
      break;
    methods.push_back(
        std::unique_ptr<FuncDefAST>(static_cast<FuncDefAST *>(def.release())));
  }
  REQUIRE(_rc, TokRightCurly, "Expected '}' to close instance",
          kw->get_location(), {nullptr, FAILED});
  return {std::make_unique<TypeClassInstanceAST>(
              kw, name_tok->lexeme, std::move(type_expr), std::move(methods)),
          SUCCESS};
}

auto Parser::expect(TokenType tokType, bool exhausts, TokenType until,
                    const std::string &message) -> std::shared_ptr<Token> {
  auto currentToken = tokStream->peek();
  auto result = !tokStream->isEnd() && currentToken->tok_type == tokType;
  if (result) {
    return tokStream->consume();
  } else {
    // TODO: Add error reporting after this point.
    if (!message.empty() && tokStream->peek()->tok_type != TokEOF) {
      this->imm_error(message, tokStream->currentLocation());
    } else if (!message.empty()) {
      this->imm_error(message, currentToken->get_location());
    }
    if (exhausts) {
      last_exhaustible_loc = currentToken->get_location();
      tokStream->exhaust_until(until);
    }

    return nullptr;
  }
}

} // namespace sammine_lang
