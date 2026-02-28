//
// Created by jjasmine on 3/7/24.
//

#pragma once
#include "lex/Token.h"
#include "util/Utilities.h"

//! \file Lexer.h
//! \brief Houses the lexer and the declaration of its method
#include <memory>

namespace sammine_lang {

//! A Lexer class with holds the core functionality Tokens and Token streams
class Lexer : public sammine_util::Reportee {
private:
  sammine_util::Location location;
  std::shared_ptr<TokenStream> tokStream;
  std::string stored_input;
  size_t cursor;
  bool at_eof;

  /// All tokens including trivia, in source order. For CST construction.
  std::vector<std::shared_ptr<Token>> raw_tokens_;

  [[nodiscard]] size_t handleNumber(size_t i, const std::string &input);
  size_t handleTrivia(size_t i, const std::string &input);
  size_t handleID(size_t i, const std::string &input);
  size_t handleInvalid(size_t i, const std::string &input);
  size_t handleOperators(size_t i, const std::string &input);
  size_t handleOperatorsADD(size_t i, const std::string &input);
  size_t handleOperatorsSUB(size_t i, const std::string &input);
  size_t handleOperatorsMUL(size_t i, const std::string &input);
  size_t handleOperatorsDIV(size_t i, const std::string &input);
  size_t handleOperatorsMOD(size_t i, const std::string &input);
  size_t handleOperatorsAND(size_t i, const std::string &input);
  size_t handleOperatorsOR(size_t i, const std::string &input);
  size_t handleOperatorsXOR(size_t i, const std::string &input);
  size_t handleOperatorsCompLeft(size_t i, const std::string &input);
  size_t handleOperatorsCompRight(size_t i, const std::string &input);
  size_t handleOperatorsEqual(size_t i, const std::string &input);
  size_t handleOperatorsNot(size_t i, const std::string &input);

  size_t handleUtility(size_t i, const std::string &input);
  size_t handleUtilityPAREN(size_t i, const std::string &input);
  size_t handleUtilityCURLY(size_t i, const std::string &input);
  size_t handleUtilityCOMMENT(size_t i, const std::string &input);
  size_t handleUtilityCOMMA(size_t i, const std::string &input);
  size_t handleUtilitySemiColon(size_t i, const std::string &input);
  size_t handleUtilityCOLON(size_t i, const std::string &input);
  size_t handleUtilityBRACKET(size_t i, const std::string &input);

  void updateLocation();

  /// Emit a token to both the filtered TokenStream (for parser) and raw_tokens_ (for CST).
  void emit(const Token &token) {
    auto ptr = std::make_shared<Token>(token);
    tokStream->push_back(ptr);
    raw_tokens_.push_back(ptr);
  }

  /// Emit a shared token to both destinations.
  void emit(const std::shared_ptr<Token> &ptr) {
    tokStream->push_back(ptr);
    raw_tokens_.push_back(ptr);
  }

public:
  explicit Lexer(const std::string &input);
  Lexer() : location(), cursor(0), at_eof(false) {
    tokStream = std::make_shared<TokenStream>();
  }
  void lexNextToken();
  std::shared_ptr<Token> peek();
  std::shared_ptr<Token> consume();

  std::shared_ptr<TokenStream> getTokenStream() const { return tokStream; }

  /// Get all tokens including trivia, in source order (for CST construction).
  const std::vector<std::shared_ptr<Token>> &getRawTokens() const {
    return raw_tokens_;
  }

  size_t advance(size_t i);

  size_t devance(size_t i);
};
} // namespace sammine_lang
