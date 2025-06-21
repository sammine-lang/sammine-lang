#pragma once
#include "util/Utilities.h"
#include <map>
#include <memory>
#include <string>

//! \file Token.h
//! \brief Defines the token structure (TokenType, TokStream, TokenMap)

namespace sammine_lang {
enum TokenType {
  // TODO: Add void keyword for return.
  TokADD, // +
  TokSUB, // -
  TokMUL, // *
  TokDIV, // /
  TokMOD, // %

  TokAddAssign, // +=
  TokAddIncr,   // ++
  TokSubAssign, // -=
  TokSubDecr,   // --
  TokMulAssign, // *=
  TokDivAssign, // /=

  TokAND,        // &&
  TokAndLogical, // &
  TokOR,         // ||
  TokORLogical,  // |
  TokXOR,        // ^
  TokSHL,        // <<
  TokSHR,        // >>

  TokEQUAL,     // ==
  TokLESS,      // <
  TokLessEqual, // <=

  TokGREATER,      // >
  TokGreaterEqual, // >=

  TokASSIGN, // =
  TokNOT,    // !

  // TokEXP AND FloorDiv
  TokEXP,      // **
  TokFloorDiv, // /_
  TokCeilDiv,  // /^

  // TokPAREN
  TokLeftParen,  // (
  TokRightParen, // )
  TokLeftCurly,  // {
  TokRightCurly, // }

  // Comma and colons and all that
  TokComma,       // ,
  TokDot,         // .
  TokSemiColon,   // ;
  TokColon,       // :
  TokDoubleColon, // ::

  // TokFunction
  TokReturn,
  TokFunc,   // fn
  TokRecord, // record
  TokArrow,  // ->
  TokLet,    // let
  TokExtern, // extern
  // TokID
  TokID,  // Representing an identifier
  TokStr, // Representing a string
  // TokNum
  TokNum,   // Representing a number
  TokTrue,  // Representing a boolean true
  TokFalse, // Representing a boolean false
  // TokIf
  TokIf,   // if
  TokElse, // else

  // TokCOMMENTS
  TokSingleComment, //
  TokEOF,
  TokINVALID,
};

static const std::map<TokenType, std::string> TokenMap = {
    {TokADD, "+"},
    {TokSUB, "-"},
    {TokMUL, "*"},
    {TokDIV, "/"},
    {TokMOD, "%"},

    {TokAddAssign, "+="},
    {TokAddIncr, "++"},
    {TokSubAssign, "-="},
    {TokSubDecr, "--"},
    {TokMulAssign, "*="},
    {TokDivAssign, "/="},

    {TokAND, "&&"},
    {TokAndLogical, "&"},
    {TokOR, "||"},
    {TokORLogical, "|"},
    {TokXOR, "^"},
    {TokSHL, ">>"},
    {TokSHR, "<<"},
    {TokEQUAL, "=="},
    {TokLESS, "<"},
    {TokLessEqual, "<="},

    {TokGREATER, ">"},
    {TokGreaterEqual, ">="},

    {TokASSIGN, "="},
    {TokNOT, "!"},
    {TokEXP, "**"},
    {TokFloorDiv, "/_"},
    {TokCeilDiv, "/^"},

    {TokLeftParen, "("},
    {TokRightParen, ")"},
    {TokLeftCurly, "{"},
    {TokRightCurly, "}"},

    {TokComma, ","},
    {TokDot, "."},
    {TokSemiColon, ";"},
    {TokColon, ":"},
    {TokDoubleColon, "::"},

    // TokFunction
    {TokReturn, "return"},
    {TokFunc, "fn"},
    {TokArrow, "->"},
    {TokLet, "let"},
    {TokRecord, "Record"},
    {TokID, "identifier"},

    {TokNum, "number"},

    {TokIf, "if"},
    {TokElse, "else"},

    // TokCOMMENTS
    {TokSingleComment, "#"},
    {TokEOF, "EOF"},
    {TokINVALID, "UNRECOGNIZED"},
};

//! A class representing a token for sammine-lang, includes TokenType, lexeme
//! and position pair as its members.

//! .
//! .
class Token {
  using Location = sammine_util::Location;

public:
  TokenType tok_type;
  std::string lexeme;
  Location location;
  Token() = delete;
  Token(TokenType type, std::string lexeme, Location location)
      : tok_type(type), lexeme(std::move(lexeme)), location(location) {}
  bool is_comparison() {
    return tok_type == TokLESS || tok_type == TokGreaterEqual ||
           tok_type == TokLessEqual || tok_type == TokGREATER ||
           tok_type == TokEQUAL;
  }
  bool is_logical() { return tok_type == TokOR || tok_type == TokAND; }
  Location get_location() const { return this->location; }
};

//! A helper class for Lexer to simplify the process of getting a token.

//!
//!
class TokenStream {
  std::vector<std::shared_ptr<Token>> TokStream;
  size_t current_index;
  bool error;

public:
  std::vector<std::shared_ptr<Token>> ErrStream;

  TokenStream() : TokStream(), current_index(0), error(false) {}

  void push_back(const std::shared_ptr<Token> &token) {
    if (token->tok_type == TokINVALID) {
      error = true;
      ErrStream.push_back(token);
    } else {
      TokStream.push_back(token);
    }
  }

  bool hasErrors() { return error; }

  void push_back(const Token &token) {
    this->push_back(std::make_shared<Token>(token));
  }

  std::shared_ptr<Token> &exhaust_until(TokenType tokType) {
    if (tokType == TokenType::TokEOF) {
      current_index = TokStream.size() - 1;
      return TokStream.back();
    }
    while (!isEnd()) {
      if (TokStream[current_index]->tok_type == tokType)
        return TokStream[current_index++];
      else
        current_index++;
    }

    return TokStream.back();
  }

  bool isEnd() { return current_index >= (TokStream.size() - 1); }
  std::shared_ptr<Token> peek() { return TokStream[current_index]; };
  std::shared_ptr<Token> consume() {
    auto token = peek();
    current_index = std::min(TokStream.size() - 1, current_index + 1);
    return token;
  }

  sammine_util::Location currentLocation() {
    if (!TokStream.empty()) {
      return TokStream[current_index]->get_location();
    }
    return {};
  }
};
} // namespace sammine_lang
