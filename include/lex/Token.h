#pragma once
#include "util/Utilities.h"
#include <functional>
#include <map>
#include <memory>
#include <string>

//! \file Token.h
//! \brief Defines the token structure (TokenType, TokStream, TokenMap)

namespace sammine_lang {
enum TokenType {
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
  TokPipe,       // |>
  TokXOR,        // ^
  TokSHL,        // <<
  TokSHR,        // >>

  TokEQUAL,     // ==
  TokLESS,      // <
  TokLessEqual, // <=

  TokGREATER,      // >
  TokGreaterEqual, // >=

  TokASSIGN,   // =
  TokNOT,      // !
  TokNOTEqual, // !=

  // TokEXP AND FloorDiv
  TokEXP,      // **
  TokFloorDiv, // /_
  TokCeilDiv,  // /^

  // TokPAREN
  TokLeftParen,    // (
  TokRightParen,   // )
  TokLeftCurly,    // {
  TokRightCurly,   // }
  TokLeftBracket,  // [
  TokRightBracket, // ]

  // Comma and colons and all that
  TokComma,       // ,
  TokDot,         // .
  TokSemiColon,   // ;
  TokColon,       // :
  TokDoubleColon, // ::

  // TokFunction
  TokReturn,
  TokFunc,   // fn
  TokStruct, // struct
  TokPtr,    // ptr
  TokAlloc,  // alloc
  TokFree,   // free
  TokLen,    // len
  TokArrow,  // ->
  TokLet,    // let
  TokMUT,    // mut
  TokReuse,  // reuse
  TokExport, // export
  TokImport, // import
  TokAs,        // as
  TokEllipsis,  // ...
  TokTypeclass, // typeclass
  TokInstance,  // instance
  TokSizeof,    // sizeof
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

  TokType, // Type

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
    {TokPipe, "|>"},
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
    {TokNOTEqual, "!="},
    {TokEXP, "**"},
    {TokFloorDiv, "/_"},
    {TokCeilDiv, "/^"},

    {TokLeftParen, "("},
    {TokRightParen, ")"},
    {TokLeftCurly, "{"},
    {TokRightCurly, "}"},
    {TokLeftBracket, "["},
    {TokRightBracket, "]"},

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
    {TokMUT, "mut"},
    {TokStruct, "struct"},
    {TokPtr, "ptr"},
    {TokAlloc, "alloc"},
    {TokFree, "free"},
    {TokLen, "len"},
    {TokReuse, "reuse"},
    {TokExport, "export"},
    {TokImport, "import"},
    {TokAs, "as"},
    {TokEllipsis, "..."},
    {TokTypeclass, "typeclass"},
    {TokInstance, "instance"},
    {TokSizeof, "sizeof"},
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
  bool is_comparison() const {
    return tok_type == TokLESS || tok_type == TokGreaterEqual ||
           tok_type == TokLessEqual || tok_type == TokGREATER ||
           tok_type == TokEQUAL || tok_type == TokNOTEqual;
  }
  bool is_assign() const { return tok_type == TokASSIGN; }
  bool is_logical() const { return tok_type == TokOR || tok_type == TokAND; }
  Location get_location() const { return this->location; }
};

//! A helper class for Lexer to simplify the process of getting a token.

//!
//!
class TokenStream {
  std::vector<std::shared_ptr<Token>> TokStream;
  size_t current_index;
  size_t rollback_mark;
  bool error;
  std::function<void()> tokenProducer;

  void ensureTokenAt(size_t index) {
    while (index >= TokStream.size() && tokenProducer)
      tokenProducer();
  }

public:
  std::vector<std::shared_ptr<Token>> ErrStream;

  TokenStream()
      : TokStream(), current_index(0), rollback_mark(0), error(false) {}

  void setTokenProducer(std::function<void()> producer) {
    tokenProducer = std::move(producer);
  }

  void push_back(const std::shared_ptr<Token> &token) {
    if (token->tok_type == TokINVALID) {
      error = true;
      ErrStream.push_back(token);
    } else {
      TokStream.push_back(token);
    }
  }

  bool hasErrors() const { return error; }

  void push_back(const Token &token) {
    this->push_back(std::make_shared<Token>(token));
  }
  void mark_rollback() { this->rollback_mark = current_index; }
  void rollback() { this->current_index = rollback_mark; }

  void rollback(size_t rollback_count) {
    assert(current_index >= rollback_count &&
           "Current index needs to be larger than rollback count");
    this->current_index -= rollback_count;
  }

  std::shared_ptr<Token> &exhaust_until(TokenType tokType) {
    if (tokType == TokenType::TokEOF) {
      // Lex everything until EOF is produced
      while (tokenProducer) {
        if (!TokStream.empty() && TokStream.back()->tok_type == TokEOF)
          break;
        tokenProducer();
      }
      current_index = TokStream.size() - 1;
      return TokStream.back();
    }
    while (!isEnd()) {
      ensureTokenAt(current_index);
      if (TokStream[current_index]->tok_type == tokType)
        return TokStream[current_index++];
      else
        current_index++;
    }

    return TokStream.back();
  }

  bool isEnd() {
    ensureTokenAt(current_index);
    return current_index < TokStream.size() &&
           TokStream[current_index]->tok_type == TokEOF;
  }
  std::shared_ptr<Token> peek() {
    ensureTokenAt(current_index);
    return TokStream[current_index];
  }
  std::shared_ptr<Token> consume() {
    ensureTokenAt(current_index);
    auto token = TokStream[current_index];
    if (token->tok_type != TokEOF)
      current_index++;
    return token;
  }

  sammine_util::Location currentLocation() {
    ensureTokenAt(current_index);
    if (!TokStream.empty()) {
      return TokStream[current_index]->get_location();
    }
    return {};
  }
};
} // namespace sammine_lang
