
#include "parser/Parser.h"
#include "ast/Ast.h"
#include "fmt/format.h"
#include "lex/Token.h"
#include <functional>
#include <memory>
#include <utility>
//! \file Parser.cpp
//! \brief Implementation for Parser class, it takes in the token stream and
//! converts it into Parsing things suchs as programs, top-level (record,
//! functions, global variables)

namespace sammine_lang {
using namespace AST;
//! \brief Holds the precedence of a binary operation
static std::map<TokenType, int> binopPrecedence = {
    {TokenType::TokASSIGN, 2}, {TokenType::TokLESS, 10},
    {TokenType::TokEQUAL, 10}, {TokenType::TokADD, 20},
    {TokenType::TokSUB, 20},   {TokenType::TokMUL, 40},
    {TokenType::TokOR, 1},
};

int GetTokPrecedence(TokenType tokType) {
  int TokPrec = binopPrecedence[tokType];
  if (TokPrec <= 0)
    return -1;
  return TokPrec;
}

auto Parser::Parse() -> u<ProgramAST> { return ParseProgram(); }

auto Parser::ParseProgram() -> u<ProgramAST> {
  auto programAST = std::make_unique<ProgramAST>();
  while (!tokStream->isEnd()) {
    auto [def, result] = ParseDefinition();
    switch (result) {
    case SUCCESS:
      programAST->DefinitionVec.push_back(std::move(def));
      continue;
    case COMMITTED_EMIT_MORE_ERROR: {
      this->error("Failed to parse a definition");
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
    this->add_error(Location::NonPrintable(),
                    "Failed to parse the remaining of file");
  }
  return programAST;
}

auto Parser::ParseDefinition() -> p<DefinitionAST> {
  using ParseFunction = std::function<p<DefinitionAST>(Parser *)>;
  std::vector<std::pair<ParseFunction, bool>> ParseFunctions = {
      {&Parser::ParseRecordDef, false},
      {&Parser::ParseFuncDef, false},
  };

  for (auto [fn, required] : ParseFunctions) {
    auto [def, result] = fn(this);
    switch (result) {
    case SUCCESS:
      return make_pair(std::move(def), result);
    case COMMITTED_EMIT_MORE_ERROR: {
      this->error("Failed to parse an extern/function definition");
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

auto Parser::ParseRecordDef() -> p<DefinitionAST> {
  auto record_tok = expect(TokRecord);
  if (!record_tok)
    return {nullptr, NONCOMMITTED};

  auto id = expect(TokID);
  if (!id) {
    this->add_error(record_tok->get_location(),
                    "Failed to parse an identifier after token Record");
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }

  auto left_curly = expect(TokLeftCurly);
  if (!left_curly) {
    this->add_error(record_tok->get_location() | id->get_location(),
                    fmt::format("Failed to parse the left curly braces for "
                                "record definition after identifier {}",
                                id->lexeme));
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }

  std::vector<std::unique_ptr<TypedVarAST>> record_members;
  while (!tokStream->isEnd()) {
    auto [member, result] = ParseTypedVar();
    switch (result) {
    case SUCCESS: {
      record_members.push_back(std::move(member));
      // TODO:
      // Later in the future we have to find a way to compose Record
      // where the last member not needing a comma(,)
      //
      // name_1 is last in this case `Record id { name_1 };`
      // name_2 is last in this case `Record id { name_1, name_2 };`
      //
      // for now we'll stick to `Record id { name_1, name_2, };`
      if (!expect(TokComma)) {
        this->add_error(
            member->get_location(),
            fmt::format("Failed to parse a comma after the Identifier {}",
                        member->name));
        return {std::make_unique<RecordDefAST>(id, std::move(record_members)),
                COMMITTED_NO_MORE_ERROR};
      }
      continue;
    }

    case NONCOMMITTED:
      break;
    case COMMITTED_EMIT_MORE_ERROR:
      this->add_error(
          record_tok->get_location(),
          fmt::format("Failed to parse record {}", record_tok->lexeme));
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      return {std::make_unique<RecordDefAST>(id, std::move(record_members)),
              COMMITTED_NO_MORE_ERROR};
    }
    break;
  }

  auto right_curly = expect(TokRightCurly);
  if (!right_curly) {
    // pick the error location
    auto err_loc = record_members.empty()
                       ? left_curly->get_location()
                       : record_members.back()->get_location();

    // build the message
    auto msg = fmt::format("Failed to parse the right curly braces at end of "
                           "record '{}' definition, "
                           "after identifier '{}'",
                           record_tok->lexeme, record_members.back()->name);

    // In the case there's no members in the Record
    if (record_members.empty()) {
      msg = fmt::format("Failed to parse the right curly braces at end of "
                        "record '{}' definition",
                        record_tok->lexeme);
    }

    this->add_error(err_loc, msg);

    return {std::make_unique<RecordDefAST>(id, std::move(record_members)),
            COMMITTED_NO_MORE_ERROR};
  }
  if (!expect(TokSemiColon))
    this->add_error(right_curly->get_location(),
                    "Failed to parse semicolon for record "
                    "definition after right curly braces");

  return {std::make_unique<RecordDefAST>(id, std::move(record_members)),
          SUCCESS};
}

auto Parser::ParseFuncDef() -> p<DefinitionAST> {
  // this is for extern
  if (auto extern_fn = expect(TokenType::TokExtern)) {

    auto [prototype, result] = ParsePrototype();
    switch (result) {
    case SUCCESS: {
      auto semi_colon = expect(TokenType::TokSemiColon);
      if (!expect(TokSemiColon))
        this->error("Need semicolon when parsing extern");
      return std::make_pair(std::make_unique<ExternAST>(std::move(prototype)),
                            SUCCESS);
    }
    case COMMITTED_EMIT_MORE_ERROR:
    case NONCOMMITTED:
      this->error("Failed to parse a prototype of a function");
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      auto result = expect(TokenType::TokSemiColon);
      return std::make_pair(std::make_unique<ExternAST>(std::move(prototype)),
                            COMMITTED_NO_MORE_ERROR);
    }
  }

  // this is for fn
  auto fn = expect(TokenType::TokFunc);
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
    this->error("Failed to parse a prototype of a function");
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    auto proto_result = expect(TokenType::TokRightCurly, /*exhausts=*/true);
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
    this->error("Failed to parse a block of a function definition");
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    auto result = expect(TokenType::TokRightCurly, /*exhausts=*/true);
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
  auto [typedVar, tv_result] = ParseTypedVar();
  switch (tv_result) {
  case SUCCESS: {
    auto assign_tok = expect(TokenType::TokASSIGN, true, TokSemiColon);
    if (!assign_tok) {
      this->error("Failed to match assig token `=`");
      return std::make_pair(
          std::make_unique<VarDefAST>(let, std::move(typedVar), nullptr),
          COMMITTED_NO_MORE_ERROR);
    }
    auto [expr, result] = ParseExpr();
    switch (result) {
    case SUCCESS: {

      auto semicolon = expect(TokenType::TokSemiColon, true);
      if (semicolon)
        return std::make_pair(std::make_unique<VarDefAST>(
                                  let, std::move(typedVar), std::move(expr)),
                              SUCCESS);

      this->error("Failed to parse semicolon after expression in a variable "
                  "definintion");
      return std::make_pair(std::make_unique<VarDefAST>(
                                let, std::move(typedVar), std::move(expr)),
                            COMMITTED_NO_MORE_ERROR);
    }

    case COMMITTED_NO_MORE_ERROR:
      [[fallthrough]];
    case COMMITTED_EMIT_MORE_ERROR:
      [[fallthrough]];
    case NONCOMMITTED:
      this->error("Failed to parse expr for variable definition");
      return std::make_pair(std::make_unique<VarDefAST>(
                                let, std::move(typedVar), std::move(expr)),
                            COMMITTED_NO_MORE_ERROR);
      break;
    }

    this->abort("Should not happen");
    return {nullptr, COMMITTED_EMIT_MORE_ERROR};
  }
  case NONCOMMITTED:
    [[fallthrough]];
  case COMMITTED_EMIT_MORE_ERROR:
    this->error("Failed to parse typed variable, already consume `let` token");
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return std::make_pair(
        std::make_unique<VarDefAST>(let, std::move(typedVar), nullptr),
        COMMITTED_NO_MORE_ERROR);
  }
  this->abort("Should not happen");
  return {nullptr, COMMITTED_EMIT_MORE_ERROR};
}

auto Parser::ParseTypedVar() -> p<TypedVarAST> {
  auto name = expect(TokenType::TokID);
  if (!name) {
    return std::make_pair(std::make_unique<TypedVarAST>(nullptr, nullptr),
                          NONCOMMITTED);
  }

  auto colon = expect(TokenType::TokColon);
  if (!colon)
    return std::make_pair(std::make_unique<TypedVarAST>(name), SUCCESS);
  auto type = expect(TokenType::TokID);

  // TODO: Make this error more specific:
  // `let x : ;` should let user know that we don't fuck around and correct
  // them to `let x : f64;` or `let x : T` for some specific T
  if (!type) {
    this->error("Expected type name after token `:`", colon->get_location());
    return std::make_pair(std::make_unique<TypedVarAST>(name, type),
                          COMMITTED_NO_MORE_ERROR);
  }

  return std::make_pair(std::make_unique<TypedVarAST>(name, type), SUCCESS);
}
auto Parser::ParsePrimaryExpr() -> p<ExprAST> {
  using ParseFunction = std::function<p<ExprAST>(Parser *)>;
  std::vector<std::pair<ParseFunction, std::string>> ParseFunctions = {
      {&Parser::ParseCallExpr, "CallExpr"},
      {&Parser::ParseParenExpr, "parenthesis"},
      {&Parser::ParseIfExpr, "IfExpr"},
      {&Parser::ParseNumberExpr, "NumberExpr"},
      {&Parser::ParseBoolExpr, "BoolExpr"},
      {&Parser::ParseVariableExpr, "VariableExpr"},
      {&Parser::ParseStringExpr, "StringExpr"},
  };

  for (auto [fn, fn_name] : ParseFunctions) {

    auto [expr, result] = fn(this);
    switch (result) {
    case SUCCESS:
      return std::make_pair(std::move(expr), SUCCESS);
    case COMMITTED_NO_MORE_ERROR:
      return std::make_pair(std::move(expr), COMMITTED_NO_MORE_ERROR);
    case COMMITTED_EMIT_MORE_ERROR:
      this->error("Failed to parse " + fn_name, expr->get_location());
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
    this->error("Failed to parse a primary expression");
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
    this->error("Failed to parse the continuation of binary expression");
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
      this->error("Expected right-hand side expression after binary operator",
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
        this->error("Failed to parse nested right-hand binary expression",
                    binOpToken->get_location());
        return {nullptr, right_result};
      }
    }

    // Combine LHS and RHS
    LHS = std::make_unique<BinaryExprAST>(binOpToken, std::move(LHS),
                                          std::move(RHS));
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
    this->error("Unable to parse an expression after return statement");
    return {std::make_unique<ReturnExprAST>(return_tok, nullptr),
            COMMITTED_NO_MORE_ERROR};
  }
  auto semi = expect(TokenType::TokSemiColon);
  if (!semi) {
    this->error("Missing the semicolon for the return statement",
                return_tok->get_location());
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

  auto [args, result] = ParseArguments();
  switch (result) {
  case SUCCESS:
    return {std::make_unique<CallExprAST>(id, std::move(args)), SUCCESS};
  case NONCOMMITTED:
    return {std::make_unique<VariableExprAST>(id), SUCCESS};
  case COMMITTED_EMIT_MORE_ERROR:
    this->error("Failed to parse arguments for call expression rule",
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
    this->error(
        "Failed to parse the predicate of if expression after the token `if`",
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
    this->error("Failed to parse the `then block` after the predicate",
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
  auto [else_block, else_result] = ParseBlock();

  switch (else_result) {
  case SUCCESS:
    return {std::make_unique<IfExprAST>(std::move(cond), std::move(then_block),
                                        std::move(else_block)),
            SUCCESS};
  case COMMITTED_EMIT_MORE_ERROR:
    [[fallthrough]];
  case NONCOMMITTED:
    this->error("Failed to parse the `else block` after the `else` token",
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
    this->error(
        fmt::format("Failed to parse the parameters in a function's prototype "
                    "after identifier `{}`",
                    id->lexeme),
        id->get_location());
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    return {nullptr, COMMITTED_NO_MORE_ERROR};
  }
  auto arrow = expect(TokArrow);
  if (!arrow)
    return {std::make_unique<PrototypeAST>(id, std::move(params)), SUCCESS};

  if (auto returnType = expect(TokID))
    return {std::make_unique<PrototypeAST>(id, returnType, std::move(params)),
            SUCCESS};
  else {
    this->error("Failed to parse the return type after the token `->`",
                arrow->get_location());
    return {std::make_unique<PrototypeAST>(id, returnType, std::move(params)),
            COMMITTED_EMIT_MORE_ERROR};
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
      auto semi = expect(TokenType::TokSemiColon, /*exhausts=*/true);
      if (!semi)
        this->error("Failed to parse a semicolon after an expression for a "
                    "statement in a block");
      blockAST->Statements.push_back(std::move(a));
      continue;
    }
    case NONCOMMITTED:
      break;
    case COMMITTED_EMIT_MORE_ERROR:
      this->error("Failed to parse a statement in a block");
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
      this->error("Failed to parse a variable definition in a block");
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
      this->error("Failed to parse a return expression in a block");
      [[fallthrough]];
    case COMMITTED_NO_MORE_ERROR:
      blockAST->Statements.push_back(std::move(rt));
      error = true;
      continue;
    }
    break;
  }

  auto rightCurly = expect(TokRightCurly, true, TokEOF,
                           "Failed to parse right curly of a statement block.");

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
        "Encountered error in parsing expression after left parenthesis");
    return {std::unique_ptr<UnitExprAST>(), COMMITTED_NO_MORE_ERROR};
  case NONCOMMITTED:
    break;
  }

  auto tok_right = expect(TokRightParen);
  if (!tok_right) {
    this->error("Expected ')' to match '(' for the expression",
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
  auto [tv, tv_result] = ParseTypedVar();

  std::vector<u<TypedVarAST>> vec;
  switch (tv_result) {
  case SUCCESS:
    vec.push_back(std::move(tv));
    break;
  case NONCOMMITTED: {
    auto rightParen = expect(TokRightParen, true);
    if (!rightParen) {
      this->error(
          "Failed to parse the right parenthesis of list of parameters");
      return {std::move(vec), COMMITTED_NO_MORE_ERROR};
    }
    if (error)
      return {std::move(vec), COMMITTED_NO_MORE_ERROR};
    return {std::move(vec), SUCCESS};
  }
  case COMMITTED_EMIT_MORE_ERROR:
    this->error("Failed to parse a typed variable as a parameter");
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    vec.push_back(std::move(tv));
    break;
  }
  while (!tokStream->isEnd()) {
    auto comma = expect(TokComma);
    if (comma == nullptr)
      break;

    // Report error if we find comma but cannot find typeVar
    auto [nvt, nvt_result] = ParseTypedVar();
    switch (nvt_result) {
    case SUCCESS:
      vec.push_back(std::move(nvt));
      break;
    case NONCOMMITTED: {
      auto rightParen = expect(TokRightParen, true);
      if (!rightParen) {
        this->error("Failed to parse the right parenthesis of list of "
                    "follow-up parameters");
        return {std::move(vec), COMMITTED_NO_MORE_ERROR};
      }
      if (error)
        return {std::move(vec), COMMITTED_NO_MORE_ERROR};

      return {std::move(vec), SUCCESS};
    }
    case COMMITTED_EMIT_MORE_ERROR:
      this->error("Failed to parse a follow-up parameters");
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
    this->error("Failed to parse the right parenthesis of list of parameters");
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
    this->error("Failed to parse an argument");
    [[fallthrough]];
  case COMMITTED_NO_MORE_ERROR:
    vec.push_back(std::move(first_expr));
    auto temp = expect(TokComma, true, TokComma);
    error = true;
    break;
  }
  while (!tokStream->isEnd()) {
    /*auto typeVar = ParseTypedVar();*/
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
        this->error("Failed to parse the right parenthesis of list of "
                    "follow-up arguments");
        return {std::move(vec), COMMITTED_NO_MORE_ERROR};
      }
      if (error)
        return {std::move(vec), COMMITTED_NO_MORE_ERROR};

      return {std::move(vec), SUCCESS};
    }
    case COMMITTED_EMIT_MORE_ERROR:
      this->error("Failed to parse a follow-up argument");
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
    this->error("Failed to parse the right parenthesis of list of arguments");
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
      this->error(message, tokStream->currentLocation());
    } else if (!message.empty()) {
      this->error(message);
    }
    if (exhausts)
      tokStream->exhaust_until(until);

    return nullptr;
  }
}

} // namespace sammine_lang
