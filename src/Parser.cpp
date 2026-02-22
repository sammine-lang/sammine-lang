
#include "parser/Parser.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "lex/Token.h"
#include <functional>
#include <memory>
#include <utility>
//! \file Parser.cpp
//! \brief Implementation for Parser class, it takes in the token stream and
//! converts it into Parsing things suchs as programs, top-level (struct,
//! functions, global variables)

namespace sammine_lang {
using namespace AST;
//! \brief Holds the precedence of a binary operation
static std::map<TokenType, int> binopPrecedence = {
    {TokenType::TokPipe, 1},
    {TokenType::TokASSIGN, 2},
    {TokenType::TokOR, 3},
    {TokenType::TokAND, 5},
    {TokenType::TokLESS, 10},
    {TokenType::TokLessEqual, 10},
    {TokenType::TokGreaterEqual, 10},
    {TokenType::TokGREATER, 10},
    {TokenType::TokEQUAL, 10},
    {TokenType::TokNOTEqual, 10},
    {TokenType::TokADD, 20},
    {TokenType::TokSUB, 20},
    {TokenType::TokMUL, 40},
    {TokenType::TokDIV, 40},
    {TokenType::TokMOD, 40},
};

int GetTokPrecedence(TokenType tokType) {
  int TokPrec = binopPrecedence[tokType];
  if (TokPrec <= 0)
    return -1;
  return TokPrec;
}

auto Parser::Parse() -> u<ProgramAST> { return ParseProgram(); }

auto Parser::ParseImport() -> std::optional<AST::ImportDecl> {
  auto import_tok = expect(TokImport);
  if (!import_tok)
    return std::nullopt;

  auto module_id = expect(TokID);
  if (!module_id) {
    this->imm_error("Expected module name after 'import'",
                    import_tok->get_location());
    return std::nullopt;
  }

  AST::ImportDecl decl;
  decl.module_name = module_id->lexeme;

  // 'as <alias>' is optional; when omitted, externs are injected directly
  auto as_tok = expect(TokAs);
  if (as_tok) {
    auto alias_id = expect(TokID);
    if (!alias_id) {
      this->imm_error("Expected alias after 'as' in import",
                      as_tok->get_location());
      return std::nullopt;
    }
    decl.alias = alias_id->lexeme;
    decl.location = import_tok->get_location() | alias_id->get_location();
    alias_to_module[decl.alias] = decl.module_name;
  } else {
    decl.alias = "";
    decl.location = import_tok->get_location() | module_id->get_location();
    // Auto-alias: `import math;` allows `math::add` syntax
    alias_to_module[decl.module_name] = decl.module_name;
  }

  auto semi = expect(TokSemiColon);
  if (!semi) {
    this->imm_error("Expected ';' after import statement",
                    decl.location);
  }

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
    switch (result) {
    case SUCCESS:
      programAST->DefinitionVec.push_back(std::move(def));
      continue;
    case COMMITTED_EMIT_MORE_ERROR: {
      this->imm_error("Incomplete definition",
                      tokStream->currentLocation());
      programAST->DefinitionVec.push_back(std::move(def));
      return programAST;
    }
    case COMMITTED_NO_MORE_ERROR: {
      programAST->DefinitionVec.push_back(std::move(def));
      return programAST;
    }
    case NONCOMMITTED: {
      break;
    }
    }
    break;
  }
  if (!tokStream->isEnd()) {
    this->add_error(tokStream->currentLocation(),
                    "Expected a valid declaration");
  }
  return programAST;
}

auto Parser::ParseDefinition() -> p<DefinitionAST> {
  using ParseFunction = std::function<p<DefinitionAST>(Parser *)>;
  std::vector<std::pair<ParseFunction, bool>> ParseFunctions = {
      {&Parser::ParseStructDef, false},
      {&Parser::ParseFuncDef, false},
  };

  for (auto [fn, required] : ParseFunctions) {
    auto [def, result] = fn(this);
    switch (result) {
    case SUCCESS:
      return make_pair(std::move(def), result);
    case COMMITTED_EMIT_MORE_ERROR: {
      this->imm_error("Incomplete function or extern definition",
                      tokStream->currentLocation());
      def->pe = true;
      return std::make_pair(std::move(def), COMMITTED_NO_MORE_ERROR);
    }
    case COMMITTED_NO_MORE_ERROR: {
      def->pe = true;
      return std::make_pair(std::move(def), COMMITTED_NO_MORE_ERROR);
    }
    case NONCOMMITTED: {
      continue;
    }
    }
  }
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}

auto Parser::ParseTypeDef() -> p<DefinitionAST> {
  auto type_tok = expect(TokType);
  if (!type_tok)
    return {nullptr, NONCOMMITTED};

  auto id = expect(TokID);
  if (!id) {
    this->add_error(type_tok->get_location(),
                    "Expected an identifier after 'type'");
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  return {nullptr, ParserError::SUCCESS};
}
auto Parser::ParseStructDef() -> p<DefinitionAST> {
  auto struct_tok = expect(TokStruct);
  if (!struct_tok)
    return {nullptr, NONCOMMITTED};

  auto id = expect(TokID);
  if (!id) {
    this->add_error(struct_tok->get_location(),
                    "Expected an identifier after 'struct'");
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }

  auto left_curly = expect(TokLeftCurly);
  if (!left_curly) {
    this->add_error(struct_tok->get_location() | id->get_location(),
                    fmt::format("Expected '{{{{' after struct identifier {}",
                                id->lexeme));
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }

  std::vector<std::unique_ptr<TypedVarAST>> struct_members;
  while (!tokStream->isEnd()) {
    auto [member, result] = ParseTypedVar();
    switch (result) {
    case SUCCESS: {
      struct_members.push_back(std::move(member));
      // TODO:
      // Later in the future we have to find a way to compose struct
      // where the last member not needing a comma(,)
      //
      // name_1 is last in this case `struct id { name_1 };`
      // name_2 is last in this case `struct id { name_1, name_2 };`
      //
      // for now we'll stick to `struct id { name_1, name_2, };`
      if (!expect(TokComma)) {
        this->add_error(
            member->get_location(),
            fmt::format("Expected ',' after member {}",
                        member->name));
        return {std::make_unique<StructDefAST>(id, std::move(struct_members)),
                COMMITTED_NO_MORE_ERROR};
      }
      continue;
    }

    case NONCOMMITTED:
      break;
    case COMMITTED_EMIT_MORE_ERROR:
      this->add_error(
          struct_tok->get_location(),
          fmt::format("Expected a valid member in struct '{}'", struct_tok->lexeme));
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      return {std::make_unique<StructDefAST>(id, std::move(struct_members)),
              COMMITTED_NO_MORE_ERROR};
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
    auto msg = fmt::format("Expected '}}}}' to close struct '{}' after member '{}'",
                           struct_tok->lexeme, struct_members.back()->name);

    // In the case there's no members in the struct
    if (struct_members.empty()) {
      msg = fmt::format("Expected '}}}}' to close struct '{}'",
                        struct_tok->lexeme);
    }

    this->add_error(err_loc, msg);

    return {std::make_unique<StructDefAST>(id, std::move(struct_members)),
            COMMITTED_NO_MORE_ERROR};
  }
  if (!expect(TokSemiColon))
    this->add_error(right_curly->get_location(),
                    "Expected ';' after struct definition");

  return {std::make_unique<StructDefAST>(id, std::move(struct_members)),
          SUCCESS};
}

auto Parser::ParseFuncDef() -> p<DefinitionAST> {
  // this is for extern / extern C
  if (auto extern_fn = expect(TokenType::TokExtern)) {
    // Check for `extern C` (re-exported C linkage extern)
    bool is_extern_c = false;
    if (!tokStream->isEnd() && tokStream->peek()->tok_type == TokID &&
        tokStream->peek()->lexeme == "C") {
      tokStream->consume(); // eat the "C"
      is_extern_c = true;
    }

    auto [prototype, result] = ParsePrototype();
    std::string kw = is_extern_c ? "extern C" : "extern";
    switch (result) {
    case SUCCESS: {
      if (!expect(TokSemiColon))
        this->imm_error("Expected ';' after " + kw + " declaration",
                        extern_fn->get_location());
      auto node = std::make_unique<ExternAST>(std::move(prototype));
      node->is_exposed = is_extern_c;
      return std::make_pair(std::move(node), SUCCESS);
    }
    case COMMITTED_EMIT_MORE_ERROR:
    case NONCOMMITTED:
      this->imm_error(result == NONCOMMITTED
                          ? "Expected a function prototype after '" + kw + "'"
                          : "Incomplete function prototype after '" + kw + "'",
                      extern_fn->get_location());
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      (void)expect(TokenType::TokSemiColon);
      auto node = std::make_unique<ExternAST>(std::move(prototype));
      node->is_exposed = is_extern_c;
      return std::make_pair(std::move(node), COMMITTED_NO_MORE_ERROR);
    }
  }

  // this is for fn
  auto fn = expect(TokenType::TokLet);
  if (!fn) {
    return std::make_pair(std::make_unique<FuncDefAST>(nullptr, nullptr),
                          NONCOMMITTED);
  }

  auto [prototype, proto_result] = ParsePrototype();
  switch (proto_result) {
  case SUCCESS:
    break;

  case COMMITTED_EMIT_MORE_ERROR:
  case NONCOMMITTED:
    this->imm_error(proto_result == NONCOMMITTED
                        ? "Expected a function prototype after 'let'"
                        : "Incomplete function prototype after 'let'",
                    fn->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    (void)expect(TokenType::TokRightCurly, /*exhausts=*/true);
    return std::make_pair(
        std::make_unique<FuncDefAST>(std::move(prototype), nullptr),
        COMMITTED_NO_MORE_ERROR);
  }
  auto [block, result] = ParseBlock();
  switch (result) {
  case SUCCESS:
    return std::make_pair(
        std::make_unique<FuncDefAST>(std::move(prototype), std::move(block)),
        SUCCESS);
  case NONCOMMITTED:
    [[fallthrough]];
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error(result == NONCOMMITTED
                        ? "Expected a function body block"
                        : "Incomplete function body block",
                    tokStream->currentLocation());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    (void)expect(TokenType::TokRightCurly, /*exhausts=*/true);
    return std::make_pair(
        std::make_unique<FuncDefAST>(std::move(prototype), std::move(block)),
        COMMITTED_NO_MORE_ERROR);
  }

  this->abort("Should not happen");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}

//! Parsing implementation for a variable decl/def

//! Accepts a let, continue parsing inside and (enable error reporting if
//! possible). If a `let` is not found then return a nullptr.
auto Parser::ParseVarDef() -> p<ExprAST> {
  auto let = expect(TokenType::TokLet);
  if (!let)
    return std::make_pair(
        std::make_unique<VarDefAST>(nullptr, nullptr, nullptr), NONCOMMITTED);
  auto mut_tok = expect(TokenType::TokMUT);
  bool is_mutable = mut_tok != nullptr;
  auto [typedVar, tv_result] = ParseTypedVar();
  switch (tv_result) {
  case SUCCESS: {
    auto assign_tok = expect(TokenType::TokASSIGN, true, TokSemiColon);
    if (!assign_tok) {
      this->imm_error("Expected '=' in variable definition",
                      typedVar->get_location());
      return std::make_pair(std::make_unique<VarDefAST>(
                                let, std::move(typedVar), nullptr, is_mutable),
                            COMMITTED_NO_MORE_ERROR);
    }
    auto [expr, result] = ParseExpr();
    switch (result) {
    case SUCCESS: {

      auto semicolon = expect(TokenType::TokSemiColon, true);
      if (semicolon)
        return std::make_pair(
            std::make_unique<VarDefAST>(let, std::move(typedVar),
                                        std::move(expr), is_mutable),
            SUCCESS);

      this->imm_error(
          "Expected ';' after variable definition",
          tokStream->currentLocation());
      return std::make_pair(
          std::make_unique<VarDefAST>(let, std::move(typedVar), std::move(expr),
                                      is_mutable),
          COMMITTED_NO_MORE_ERROR);
    }

    case COMMITTED_NO_MORE_ERROR:
      [[fallthrough]];
    case COMMITTED_EMIT_MORE_ERROR:
      [[fallthrough]];
    case NONCOMMITTED:
      this->imm_error(result == NONCOMMITTED
                          ? "Expected an expression in variable definition"
                          : "Incomplete expression in variable definition",
                      let->get_location());
      return std::make_pair(
          std::make_unique<VarDefAST>(let, std::move(typedVar), std::move(expr),
                                      is_mutable),
          COMMITTED_NO_MORE_ERROR);
      break;
    }

    this->abort("Should not happen");
    return {nullptr, COMMITTED_EMIT_MORE_ERROR};
  }
  case NONCOMMITTED:
    [[fallthrough]];
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error(tv_result == NONCOMMITTED
                        ? "Expected a typed variable after 'let'"
                        : "Incomplete typed variable after 'let'",
                    let->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return std::make_pair(std::make_unique<VarDefAST>(let, std::move(typedVar),
                                                      nullptr, is_mutable),
                          COMMITTED_NO_MORE_ERROR);
  }
  this->abort("Should not happen");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}

auto Parser::consumeClosingAngleBracket() -> bool {
  // Case 1: A child ParseTypeExpr already split a ">>" (TokSHR) and left
  // one '>' for us. Claim it.
  if (split_greater_depth > 0) {
    split_greater_depth--;
    return true;
  }
  // Case 2: A plain '>' token — consume it directly.
  if (expect(TokenType::TokGREATER))
    return true;
  // Case 3: ">>" token (TokSHR) — consume it, use one '>' now,
  // and save the other for our parent.
  if (expect(TokenType::TokSHR)) {
    split_greater_depth++;
    return true;
  }
  return false;
}

auto Parser::ParseTypeExprTopLevel() -> std::unique_ptr<TypeExprAST> {
  auto result = ParseTypeExpr();
  if (split_greater_depth > 0) {
    split_greater_depth = 0;
    this->imm_error("Extra '>' in type expression",
                    tokStream->currentLocation());
    return nullptr;
  }
  return result;
}

auto Parser::ParseTypeExpr() -> std::unique_ptr<TypeExprAST> {
  if (auto ptr_tok = expect(TokenType::TokPtr)) {
    if (!expect(TokenType::TokLESS)) {
      this->imm_error("Expected '<' after 'ptr'", ptr_tok->get_location());
      return nullptr;
    }
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
    if (!expect(TokenType::TokSemiColon)) {
      this->imm_error("Expected ';' after element type in '[TYPE;SIZE]'",
                      lbracket->get_location());
      return nullptr;
    }
    auto size_tok = expect(TokenType::TokNum);
    if (!size_tok) {
      this->imm_error("Expected integer size in '[TYPE;SIZE]'",
                      lbracket->get_location());
      return nullptr;
    }
    size_t arr_size = std::stoul(size_tok->lexeme);
    if (!expect(TokenType::TokRightBracket)) {
      this->imm_error("Expected ']' to close '[TYPE;SIZE]'",
                      lbracket->get_location());
      return nullptr;
    }
    auto result = std::make_unique<ArrayTypeExprAST>(std::move(elem), arr_size);
    result->location = lbracket->get_location();
    return result;
  }
  if (auto lparen = expect(TokenType::TokLeftParen)) {
    // Parse function type: (type, type, ...) -> retType
    std::vector<std::unique_ptr<TypeExprAST>> paramTypes;
    if (tokStream->peek()->tok_type != TokenType::TokRightParen) {
      auto first = ParseTypeExpr();
      if (!first) {
        this->imm_error("Expected parameter type in function type",
                        lparen->get_location());
        return nullptr;
      }
      paramTypes.push_back(std::move(first));
      while (expect(TokenType::TokComma)) {
        auto param = ParseTypeExpr();
        if (!param) {
          this->imm_error("Expected parameter type after ',' in function type",
                          lparen->get_location());
          return nullptr;
        }
        paramTypes.push_back(std::move(param));
      }
    }
    if (!expect(TokenType::TokRightParen)) {
      this->imm_error("Expected ')' in function type", lparen->get_location());
      return nullptr;
    }
    if (!expect(TokenType::TokArrow)) {
      this->imm_error("Expected '->' after ')' in function type",
                      lparen->get_location());
      return nullptr;
    }
    auto retType = ParseTypeExpr();
    if (!retType) {
      this->imm_error("Expected return type after '->' in function type",
                      lparen->get_location());
      return nullptr;
    }
    auto result = std::make_unique<FunctionTypeExprAST>(std::move(paramTypes),
                                                        std::move(retType));
    result->location = lparen->get_location();
    return result;
  }
  if (auto id = expect(TokenType::TokID)) {
    // Handle qualified type names: alias::TypeName (e.g. m::Point → math$Point)
    if (tokStream->peek()->tok_type == TokDoubleColon) {
      auto it = alias_to_module.find(id->lexeme);
      if (it != alias_to_module.end()) {
        tokStream->consume(); // consume the ::
        auto member_id = expect(TokID);
        if (!member_id) {
          this->imm_error(
              fmt::format("Expected type name after '{}::`", id->lexeme),
              id->get_location());
          return nullptr;
        }
        // Construct mangled name: module$TypeName
        member_id->lexeme = it->second + "$" + member_id->lexeme;
        return std::make_unique<SimpleTypeExprAST>(member_id);
      }
    }
    return std::make_unique<SimpleTypeExprAST>(id);
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
      return std::make_pair(std::make_unique<TypedVarAST>(nullptr, nullptr),
                            COMMITTED_NO_MORE_ERROR);
    }
    return std::make_pair(std::make_unique<TypedVarAST>(nullptr, nullptr),
                          NONCOMMITTED);
  }

  auto colon = expect(TokenType::TokColon);
  if (!colon)
    return std::make_pair(std::make_unique<TypedVarAST>(name, is_mutable),
                          SUCCESS);
  auto type_expr = ParseTypeExprTopLevel();

  if (!type_expr) {
    this->imm_error("Expected type name after token `:`",
                    colon->get_location());
    return std::make_pair(
        std::make_unique<TypedVarAST>(name, std::move(type_expr), is_mutable),
        COMMITTED_NO_MORE_ERROR);
  }

  return std::make_pair(
      std::make_unique<TypedVarAST>(name, std::move(type_expr), is_mutable),
      SUCCESS);
}
auto Parser::ParseUnaryNegExpr() -> p<ExprAST> {
  auto sub_tok = expect(TokenType::TokSUB);
  if (!sub_tok)
    return {nullptr, NONCOMMITTED};
  auto [operand, result] = ParsePrimaryExpr();
  switch (result) {
  case SUCCESS:
    return {std::make_unique<UnaryNegExprAST>(sub_tok, std::move(operand)),
            SUCCESS};
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete operand after '-'",
                    sub_tok->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<UnaryNegExprAST>(sub_tok, std::move(operand)),
            COMMITTED_NO_MORE_ERROR};
  case NONCOMMITTED:
    this->imm_error("Expected expression after '-'", sub_tok->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  this->abort("Should not happen in ParseUnaryNegExpr");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
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
  switch (result) {
  case SUCCESS:
    return {std::make_unique<DerefExprAST>(star_tok, std::move(operand)),
            SUCCESS};
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete operand after '*'",
                    star_tok->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<DerefExprAST>(star_tok, std::move(operand)),
            COMMITTED_NO_MORE_ERROR};
  case NONCOMMITTED:
    this->imm_error("Expected expression after '*'", star_tok->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  this->abort("Should not happen in ParseDerefExpr");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}

auto Parser::ParseAddrOfExpr() -> p<ExprAST> {
  auto amp_tok = expect(TokenType::TokAndLogical);
  if (!amp_tok)
    return {nullptr, NONCOMMITTED};
  auto [operand, result] = ParsePrimaryExpr();
  switch (result) {
  case SUCCESS:
    return {std::make_unique<AddrOfExprAST>(amp_tok, std::move(operand)),
            SUCCESS};
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete operand after '&'",
                    amp_tok->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<AddrOfExprAST>(amp_tok, std::move(operand)),
            COMMITTED_NO_MORE_ERROR};
  case NONCOMMITTED:
    this->imm_error("Expected expression after '&'", amp_tok->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  this->abort("Should not happen in ParseAddrOfExpr");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}

auto Parser::ParseAllocExpr() -> p<ExprAST> {
  auto alloc_tok = expect(TokenType::TokAlloc);
  if (!alloc_tok)
    return {nullptr, NONCOMMITTED};
  auto left_paren = expect(TokenType::TokLeftParen);
  if (!left_paren) {
    this->imm_error("Expected '(' after 'alloc'", alloc_tok->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  auto [operand, result] = ParseExpr();
  switch (result) {
  case SUCCESS:
    break;
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete expression inside 'alloc()'",
                    alloc_tok->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<AllocExprAST>(alloc_tok, std::move(operand)),
            COMMITTED_NO_MORE_ERROR};
  case NONCOMMITTED:
    this->imm_error("Expected expression inside alloc()",
                    alloc_tok->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  auto right_paren = expect(TokenType::TokRightParen);
  if (!right_paren) {
    this->imm_error("Expected ')' to close alloc()", alloc_tok->get_location());
    return {std::make_unique<AllocExprAST>(alloc_tok, std::move(operand)),
            COMMITTED_NO_MORE_ERROR};
  }
  return {std::make_unique<AllocExprAST>(alloc_tok, std::move(operand)),
          SUCCESS};
}

auto Parser::ParseFreeExpr() -> p<ExprAST> {
  auto free_tok = expect(TokenType::TokFree);
  if (!free_tok)
    return {nullptr, NONCOMMITTED};
  auto left_paren = expect(TokenType::TokLeftParen);
  if (!left_paren) {
    this->imm_error("Expected '(' after 'free'", free_tok->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  auto [operand, result] = ParseExpr();
  switch (result) {
  case SUCCESS:
    break;
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete expression inside 'free()'",
                    free_tok->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<FreeExprAST>(free_tok, std::move(operand)),
            COMMITTED_NO_MORE_ERROR};
  case NONCOMMITTED:
    this->imm_error("Expected expression inside free()",
                    free_tok->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  auto right_paren = expect(TokenType::TokRightParen);
  if (!right_paren) {
    this->imm_error("Expected ')' to close free()", free_tok->get_location());
    return {std::make_unique<FreeExprAST>(free_tok, std::move(operand)),
            COMMITTED_NO_MORE_ERROR};
  }
  return {std::make_unique<FreeExprAST>(free_tok, std::move(operand)), SUCCESS};
}

auto Parser::ParseArrayLiteralExpr() -> p<ExprAST> {
  auto left_bracket = expect(TokenType::TokLeftBracket);
  if (!left_bracket)
    return {nullptr, NONCOMMITTED};
  std::vector<u<ExprAST>> elements;
  auto [first, first_result] = ParseExpr();
  switch (first_result) {
  case SUCCESS:
    elements.push_back(std::move(first));
    break;
  case NONCOMMITTED:
    // Empty array literal [] - not supported, need at least one element
    this->imm_error("Expected at least one element in array literal",
                    left_bracket->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete element in array literal",
                    left_bracket->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)),
            COMMITTED_NO_MORE_ERROR};
  }
  while (!tokStream->isEnd()) {
    auto comma = expect(TokComma);
    if (!comma)
      break;
    auto [elem, elem_result] = ParseExpr();
    switch (elem_result) {
    case SUCCESS:
      elements.push_back(std::move(elem));
      break;
    case COMMITTED_EMIT_MORE_ERROR:
      this->imm_error("Incomplete element in array literal",
                      left_bracket->get_location());
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)),
              COMMITTED_NO_MORE_ERROR};
    case NONCOMMITTED:
      this->imm_error("Expected expression after ',' in array literal",
                      left_bracket->get_location());
      return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)),
              COMMITTED_NO_MORE_ERROR};
    }
  }
  auto right_bracket = expect(TokenType::TokRightBracket);
  if (!right_bracket) {
    this->imm_error("Expected ']' to close array literal",
                    left_bracket->get_location());
    return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)),
            COMMITTED_NO_MORE_ERROR};
  }
  return {std::make_unique<ArrayLiteralExprAST>(std::move(elements)), SUCCESS};
}

auto Parser::ParseLenExpr() -> p<ExprAST> {
  auto len_tok = expect(TokenType::TokLen);
  if (!len_tok)
    return {nullptr, NONCOMMITTED};
  auto left_paren = expect(TokenType::TokLeftParen);
  if (!left_paren) {
    this->imm_error("Expected '(' after 'len'", len_tok->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  auto [operand, result] = ParseExpr();
  switch (result) {
  case SUCCESS:
    break;
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete expression inside 'len()'",
                    len_tok->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<LenExprAST>(len_tok, std::move(operand)),
            COMMITTED_NO_MORE_ERROR};
  case NONCOMMITTED:
    this->imm_error("Expected expression inside len()",
                    len_tok->get_location());
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  auto right_paren = expect(TokenType::TokRightParen);
  if (!right_paren) {
    this->imm_error("Expected ')' to close len()", len_tok->get_location());
    return {std::make_unique<LenExprAST>(len_tok, std::move(operand)),
            COMMITTED_NO_MORE_ERROR};
  }
  return {std::make_unique<LenExprAST>(len_tok, std::move(operand)), SUCCESS};
}

auto Parser::ParsePrimaryExpr() -> p<ExprAST> {
  using ParseFunction = std::function<p<ExprAST>(Parser *)>;
  std::vector<std::pair<ParseFunction, std::string>> ParseFunctions = {
      {&Parser::ParseUnaryNegExpr, "UnaryNegExpr"},
      {&Parser::ParseDerefExpr, "DerefExpr"},
      {&Parser::ParseAddrOfExpr, "AddrOfExpr"},
      {&Parser::ParseAllocExpr, "AllocExpr"},
      {&Parser::ParseFreeExpr, "FreeExpr"},
      {&Parser::ParseLenExpr, "LenExpr"},
      {&Parser::ParseArrayLiteralExpr, "ArrayLiteralExpr"},
      {&Parser::ParseCallExpr, "CallExpr"},
      {&Parser::ParseParenExpr, "parenthesis"},
      {&Parser::ParseIfExpr, "IfExpr"},
      {&Parser::ParseNumberExpr, "NumberExpr"},
      {&Parser::ParseBoolExpr, "BoolExpr"},
      {&Parser::ParseStringExpr, "StringExpr"},
  };

  for (auto [fn, fn_name] : ParseFunctions) {

    auto [expr, result] = fn(this);
    switch (result) {
    case SUCCESS: {
      // Handle postfix operators: expr[idx] and expr.field
      while (true) {
        if (auto lb = expect(TokenType::TokLeftBracket)) {
          auto [idx, idx_result] = ParseExpr();
          switch (idx_result) {
          case SUCCESS:
            break;
          case COMMITTED_EMIT_MORE_ERROR:
            this->imm_error("Incomplete index expression inside '[]'",
                            lb->get_location());
            [[fallthrough]];
          case COMMITTED_NO_MORE_ERROR:
            return {
                std::make_unique<IndexExprAST>(std::move(expr), std::move(idx)),
                COMMITTED_NO_MORE_ERROR};
          case NONCOMMITTED:
            this->imm_error("Expected expression inside '[]'",
                            lb->get_location());
            return {nullptr, COMMITTED_NO_MORE_ERROR};
          }
          auto rb = expect(TokenType::TokRightBracket);
          if (!rb) {
            this->imm_error("Expected ']' to close index expression",
                            lb->get_location());
            return {
                std::make_unique<IndexExprAST>(std::move(expr), std::move(idx)),
                COMMITTED_NO_MORE_ERROR};
          }
          expr =
              std::make_unique<IndexExprAST>(std::move(expr), std::move(idx));
        } else if (auto dot = expect(TokenType::TokDot)) {
          auto field_tok = expect(TokenType::TokID);
          if (!field_tok) {
            this->imm_error("Expected field name after '.'",
                            dot->get_location());
            return {std::make_unique<FieldAccessExprAST>(std::move(expr),
                                                         nullptr),
                    COMMITTED_NO_MORE_ERROR};
          }
          expr = std::make_unique<FieldAccessExprAST>(std::move(expr),
                                                       field_tok);
        } else {
          break;
        }
      }
      return std::make_pair(std::move(expr), SUCCESS);
    }
    case COMMITTED_NO_MORE_ERROR:
      return std::make_pair(std::move(expr), COMMITTED_NO_MORE_ERROR);
    case COMMITTED_EMIT_MORE_ERROR:
      this->imm_error("Incomplete " + fn_name, expr->get_location());
      return {std::move(expr), COMMITTED_NO_MORE_ERROR};
    case NONCOMMITTED:
      break;
    }

    if (fn_name == ParseFunctions.back().second)
      return {nullptr, NONCOMMITTED};
  }
  this->abort("Should not happen in ParsePrimaryExpr");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}
auto Parser::ParseExpr() -> p<ExprAST> {
  auto [LHS, left_result] = ParsePrimaryExpr();
  switch (left_result) {
  case SUCCESS:
    break;
  case NONCOMMITTED:
    return {std::move(LHS), NONCOMMITTED};
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete expression",
                    tokStream->currentLocation());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::move(LHS), COMMITTED_NO_MORE_ERROR};
  }

  auto [next, right_result] = ParseBinaryExpr(0, std::move(LHS));
  switch (right_result) {
  case SUCCESS:
    return {std::move(next), SUCCESS};
  case NONCOMMITTED:
    return {std::move(next), SUCCESS};
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete right-hand side of binary expression",
                    tokStream->currentLocation());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::move(next), COMMITTED_NO_MORE_ERROR};
  }
  this->abort("should not happen in ParseExpr, call Jasmine");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}

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
      this->imm_error(
          "Expected right-hand side expression after binary operator",
          binOpToken->get_location());
      return {nullptr, COMMITTED_NO_MORE_ERROR};
    }

    // Look ahead: if the next operator binds more tightly, parse it first
    auto nextTok = tokStream->peek()->tok_type;
    int NextPrec = GetTokPrecedence(nextTok);
    if (TokPrec < NextPrec) {
      std::tie(RHS, right_result) =
          ParseBinaryExpr(TokPrec + 1, std::move(RHS));
      if (right_result != SUCCESS) {
        this->imm_error("Incomplete right-hand expression after binary operator",
                        binOpToken->get_location());
        return {nullptr, right_result};
      }
    }

    // Combine LHS and RHS
    if (binOpToken->tok_type == TokenType::TokPipe) {
      // x |> f      →  f(x)
      // x |> f(y,z) →  f(x,y,z)
      std::shared_ptr<Token> tok;
      std::vector<std::unique_ptr<ExprAST>> new_args;

      if (auto *call = dynamic_cast<CallExprAST *>(RHS.get())) {
        tok = std::make_shared<Token>(TokenType::TokID, call->functionName,
                                     call->get_location());
        for (auto &arg : call->arguments)
          new_args.push_back(std::move(arg));
      } else if (auto *var = dynamic_cast<VariableExprAST *>(RHS.get())) {
        tok = std::make_shared<Token>(TokenType::TokID, var->variableName,
                                     var->get_location());
      } else {
        this->imm_error("Right-hand side of |> must be a function name or call",
                        binOpToken->get_location());
        return {nullptr, COMMITTED_NO_MORE_ERROR};
      }

      new_args.insert(new_args.begin(), std::move(LHS));
      LHS = std::make_unique<CallExprAST>(tok, std::move(new_args));
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
  switch (result) {
  case SUCCESS:
    break;
  case NONCOMMITTED:
    break;
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<ReturnExprAST>(return_tok, std::move(expr)),
            COMMITTED_NO_MORE_ERROR};
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete expression after 'return'",
                    return_tok->get_location());
    return {std::make_unique<ReturnExprAST>(return_tok, nullptr),
            COMMITTED_NO_MORE_ERROR};
  }
  auto semi = expect(TokenType::TokSemiColon);
  if (!semi) {
    this->imm_error("Expected ';' after return statement",
                    return_tok ? return_tok->get_location()
                               : this->tokStream->peek()->get_location());
    return {std::make_unique<ReturnExprAST>(return_tok, std::move(expr)),
            COMMITTED_NO_MORE_ERROR};
  }

  if (result == SUCCESS)
    return {std::make_unique<ReturnExprAST>(return_tok, std::move(expr)),
            SUCCESS};
  else if (result == NONCOMMITTED)
    return {std::make_unique<ReturnExprAST>(return_tok,
                                            std::make_unique<UnitExprAST>()),
            SUCCESS};
  else
    this->abort("Impossible, logic error in the parser of returnexpr");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}

auto Parser::ParseCallExpr() -> p<ExprAST> {

  auto id = expect(TokenType::TokID);
  if (!id)
    return {std::make_unique<CallExprAST>(nullptr), NONCOMMITTED};

  // Handle qualified names: alias::member (e.g. m::add → math$add)
  if (tokStream->peek()->tok_type == TokDoubleColon) {
    auto it = alias_to_module.find(id->lexeme);
    if (it != alias_to_module.end()) {
      tokStream->consume(); // consume the ::
      auto member_id = expect(TokID);
      if (!member_id) {
        this->imm_error(
            fmt::format("Expected member name after '{}::`", id->lexeme),
            id->get_location());
        return {nullptr, COMMITTED_NO_MORE_ERROR};
      }
      // Construct mangled name: module$func
      member_id->lexeme = it->second + "$" + member_id->lexeme;
      id = member_id;
    }
  }

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

  if (is_struct_literal) {
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
      if (val_result == COMMITTED_EMIT_MORE_ERROR ||
          val_result == COMMITTED_NO_MORE_ERROR) {
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

    auto rbrace = expect(TokRightCurly);
    if (!rbrace) {
      this->imm_error("Expected '}' to close struct literal",
                      lbrace->get_location());
      return {std::make_unique<StructLiteralExprAST>(
                  id, std::move(field_names), std::move(field_values)),
              COMMITTED_NO_MORE_ERROR};
    }
    if (had_error) {
      return {std::make_unique<StructLiteralExprAST>(
                  id, std::move(field_names), std::move(field_values)),
              COMMITTED_NO_MORE_ERROR};
    }
    return {std::make_unique<StructLiteralExprAST>(
                id, std::move(field_names), std::move(field_values)),
            SUCCESS};
  }

  auto [args, result] = ParseArguments();
  switch (result) {
  case SUCCESS:
    return {std::make_unique<CallExprAST>(id, std::move(args)), SUCCESS};
  case NONCOMMITTED:
    return {std::make_unique<VariableExprAST>(id), SUCCESS};
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete arguments for function call",
                    id->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<CallExprAST>(id, std::move(args)),
            COMMITTED_NO_MORE_ERROR};
  }
  this->abort("Should not happen");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}

auto Parser::ParseIfExpr() -> p<ExprAST> {
  auto if_tok = expect(TokenType::TokIf);

  if (!if_tok)
    return {std::make_unique<IfExprAST>(nullptr, nullptr, nullptr),
            NONCOMMITTED};

  auto [cond, cond_result] = ParseExpr();

  switch (cond_result) {
  case SUCCESS:
    break;
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<IfExprAST>(std::move(cond), nullptr, nullptr),
            COMMITTED_NO_MORE_ERROR};
  case COMMITTED_EMIT_MORE_ERROR:
    [[fallthrough]];
  case NONCOMMITTED:
    this->imm_error(cond_result == NONCOMMITTED
                        ? "Expected a condition after 'if'"
                        : "Incomplete condition after 'if'",
                    if_tok->get_location());
    return {std::make_unique<IfExprAST>(std::move(cond), nullptr, nullptr),
            COMMITTED_NO_MORE_ERROR};
  }

  auto [then_block, then_result] = ParseBlock();
  switch (then_result) {
  case SUCCESS:
    break;
  case COMMITTED_EMIT_MORE_ERROR:
    [[fallthrough]];
  case NONCOMMITTED:
    this->imm_error(then_result == NONCOMMITTED
                        ? "Expected a block after 'if' condition"
                        : "Incomplete block after 'if' condition",
                    cond->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<IfExprAST>(std::move(cond), std::move(then_block),
                                        nullptr),
            COMMITTED_NO_MORE_ERROR};
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

  switch (else_result) {
  case SUCCESS:
    return {std::make_unique<IfExprAST>(std::move(cond), std::move(then_block),
                                        std::move(else_block)),
            SUCCESS};
  case COMMITTED_EMIT_MORE_ERROR:
    [[fallthrough]];
  case NONCOMMITTED:
    this->imm_error(else_result == NONCOMMITTED
                        ? "Expected a block after 'else'"
                        : "Incomplete block after 'else'",
                    else_tok->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {std::make_unique<IfExprAST>(std::move(cond), std::move(then_block),
                                        std::move(else_block)),
            COMMITTED_NO_MORE_ERROR};
  }
  this->abort("Should not happen");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
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
  else
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

  auto [params, result] = ParseParams();
  switch (result) {
  case SUCCESS:
    break;
  case COMMITTED_EMIT_MORE_ERROR:
    [[fallthrough]];
  case NONCOMMITTED:
    this->imm_error(
        result == NONCOMMITTED
            ? fmt::format("Expected '(' for parameter list after '{}'",
                          id->lexeme)
            : fmt::format("Incomplete parameter list for '{}'", id->lexeme),
        id->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  bool var_arg = parsed_var_arg;

  auto arrow = expect(TokArrow);
  if (!arrow) {
    auto proto = std::make_unique<PrototypeAST>(id, std::move(params));
    proto->is_var_arg = var_arg;
    return {std::move(proto), SUCCESS};
  }

  auto return_type_expr = ParseTypeExprTopLevel();
  if (return_type_expr) {
    auto proto = std::make_unique<PrototypeAST>(
        id, std::move(return_type_expr), std::move(params));
    proto->is_var_arg = var_arg;
    return {std::move(proto), SUCCESS};
  } else {
    this->imm_error("Expected a return type after '->'",
                    arrow->get_location());
    auto proto = std::make_unique<PrototypeAST>(
        id, std::move(return_type_expr), std::move(params));
    proto->is_var_arg = var_arg;
    return {std::move(proto), COMMITTED_EMIT_MORE_ERROR};
  }
}

auto Parser::ParseBlock() -> p<BlockAST> {
  bool error = false;
  auto leftCurly = expect(TokLeftCurly);
  if (!leftCurly)
    return {nullptr, NONCOMMITTED};

  auto blockAST = std::make_unique<BlockAST>();
  while (!tokStream->isEnd()) {
    auto [a, a_result] = ParseExpr();
    switch (a_result) {
    case SUCCESS: {
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
    case NONCOMMITTED:
      break;
    case COMMITTED_EMIT_MORE_ERROR:
      this->imm_error("Incomplete statement in block",
                      tokStream->currentLocation());
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      if (a)
        blockAST->Statements.push_back(std::move(a));
      auto semi = expect(TokenType::TokSemiColon, /*exhausts=*/true);
      error = true;
      semi = expect(TokSemiColon);
      continue;
    }

    auto [b, b_result] = ParseVarDef();

    switch (b_result) {
    case SUCCESS: {
      blockAST->Statements.push_back(std::move(b));
      continue;
    }
    case NONCOMMITTED:
      break;
    case COMMITTED_EMIT_MORE_ERROR:
      this->imm_error("Incomplete variable definition in block",
                      tokStream->currentLocation());
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      if (b)
        blockAST->Statements.push_back(std::move(b));
      error = true;
      continue;
    }

    auto [rt, rt_result] = ParseReturnExpr();
    switch (rt_result) {
    case SUCCESS:
      blockAST->Statements.push_back(std::move(rt));
      continue;
    case NONCOMMITTED:
      break;

    case COMMITTED_EMIT_MORE_ERROR:
      this->imm_error("Incomplete return expression in block",
                      tokStream->currentLocation());
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      blockAST->Statements.push_back(std::move(rt));
      error = true;
      continue;
    }
    break;
  }

  auto rightCurly = expect(TokRightCurly, true, TokEOF,
                           "Expected '}' to close statement block");

  if (!rightCurly)
    return {std::move(blockAST), COMMITTED_NO_MORE_ERROR};
  if (error)
    return {std::move(blockAST), COMMITTED_NO_MORE_ERROR};
  else
    return {std::move(blockAST), SUCCESS};
}

auto Parser::ParseParenExpr() -> p<ExprAST> {
  auto tok_left = expect(TokLeftParen);
  if (!tok_left) {
    return {nullptr, NONCOMMITTED};
  }

  auto [expr, result] = ParseExpr(); // Parse inner expression
  switch (result) {
  case SUCCESS:
    break;
  case COMMITTED_EMIT_MORE_ERROR:
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    this->add_error(
        tok_left->get_location(),
        "Incomplete expression after '('");
    return {std::unique_ptr<UnitExprAST>(), COMMITTED_NO_MORE_ERROR};
  case NONCOMMITTED:
    break;
  }

  auto tok_right = expect(TokRightParen);
  if (!tok_right) {
    this->imm_error("Expected ')' to match '(' for the expression",
                    tok_left->get_location() | expr->get_location());
    return {std::move(expr), COMMITTED_NO_MORE_ERROR};
  }

  if (result == SUCCESS)
    return {std::move(expr), SUCCESS};
  else if (result == NONCOMMITTED)
    return {std::make_unique<UnitExprAST>(tok_left, tok_right), SUCCESS};

  else
    this->abort("oops");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}
// Parsing of parameters in a function call, we use leftParen and rightParen
// as appropriate stopping point
auto Parser::ParseParams()
    -> std::pair<std::vector<u<TypedVarAST>>, ParserError> {
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
      this->imm_error(
          "Expected ')' after '...'",
          leftParen->get_location());
      return {std::vector<u<TypedVarAST>>(), COMMITTED_NO_MORE_ERROR};
    }
    return {std::vector<u<TypedVarAST>>(), SUCCESS};
  }

  auto [tv, tv_result] = ParseTypedVar();

  std::vector<u<TypedVarAST>> vec;
  switch (tv_result) {
  case SUCCESS:
    vec.push_back(std::move(tv));
    break;
  case NONCOMMITTED: {
    auto rightParen = expect(TokRightParen, true);
    if (!rightParen) {
      this->imm_error(
          "Expected ')' after parameters",
          leftParen->get_location());
      return {std::move(vec), COMMITTED_NO_MORE_ERROR};
    }
    if (error)
      return {std::move(vec), COMMITTED_NO_MORE_ERROR};
    return {std::move(vec), SUCCESS};
  }
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete parameter declaration",
                    leftParen->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    vec.push_back(std::move(tv));
    break;
  }
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
    switch (nvt_result) {
    case SUCCESS:
      vec.push_back(std::move(nvt));
      break;
    case NONCOMMITTED: {
      auto rightParen = expect(TokRightParen, true);
      if (!rightParen) {
        this->imm_error("Expected ')' after parameters",
                        leftParen->get_location());
        return {std::move(vec), COMMITTED_NO_MORE_ERROR};
      }
      if (error)
        return {std::move(vec), COMMITTED_NO_MORE_ERROR};

      return {std::move(vec), SUCCESS};
    }
    case COMMITTED_EMIT_MORE_ERROR:
      this->imm_error("Incomplete parameter after ','",
                      leftParen->get_location());
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      vec.push_back(std::move(nvt));
      auto temp = expect(TokComma, true, TokComma);
      error = true;
      continue;
    }
  }
  auto rightParen = expect(TokRightParen, true);
  if (!rightParen) {
    this->imm_error(
        "Expected ')' after parameters",
        leftParen->get_location());
    return {std::move(vec), COMMITTED_NO_MORE_ERROR};
  }
  if (error)
    return {std::move(vec), COMMITTED_NO_MORE_ERROR};

  return {std::move(vec), SUCCESS};
}

auto Parser::ParseArguments()
    -> std::pair<std::vector<u<ExprAST>>, ParserError> {
  auto leftParen = expect(TokLeftParen);
  bool error = false;
  if (!leftParen)
    return {std::vector<u<ExprAST>>(), NONCOMMITTED};

  auto vec = std::vector<u<ExprAST>>();

  auto [first_expr, first_result] = ParseExpr();
  switch (first_result) {
  case SUCCESS:
    vec.push_back(std::move(first_expr));
    break;
  case NONCOMMITTED:
    break;
  case COMMITTED_EMIT_MORE_ERROR:
    this->imm_error("Incomplete argument", leftParen->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    vec.push_back(std::move(first_expr));
    auto temp = expect(TokComma, true, TokComma);
    error = true;
    break;
  }
  while (!tokStream->isEnd()) {
    auto comma = expect(TokComma);
    if (comma == nullptr)
      break;

    auto [expr, expr_result] = ParseExpr();
    switch (expr_result) {
    case SUCCESS:
      vec.push_back(std::move(expr));
      break;
    case NONCOMMITTED: {
      auto rightParen = expect(TokRightParen, true);
      if (!rightParen) {
        this->imm_error("Expected ')' after arguments",
                        leftParen->get_location());
        return {std::move(vec), COMMITTED_NO_MORE_ERROR};
      }
      if (error)
        return {std::move(vec), COMMITTED_NO_MORE_ERROR};

      return {std::move(vec), SUCCESS};
    }
    case COMMITTED_EMIT_MORE_ERROR:
      this->imm_error("Incomplete argument after ','",
                      leftParen->get_location());
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      vec.push_back(std::move(first_expr));
      auto temp = expect(TokComma, true, TokComma);
      error = true;
      continue;
    }
  }
  auto rightParen = expect(TokRightParen, true);
  if (!rightParen) {
    this->imm_error(
        "Expected ')' after arguments",
        leftParen->get_location());
    return {std::move(vec), COMMITTED_NO_MORE_ERROR};
  }
  if (error)
    return {std::move(vec), COMMITTED_NO_MORE_ERROR};

  return {std::move(vec), SUCCESS};
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
