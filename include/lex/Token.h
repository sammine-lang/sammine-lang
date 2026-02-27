#pragma once
#include "util/Utilities.h"
#include <cassert>
#include <functional>
#include <list>
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
  TokEnum,      // enum
  TokCase,      // case
  TokFatArrow,  // =>
  // TokID
  TokID,  // Representing an identifier
  TokStr, // Representing a string
  // TokNum
  TokNum,   // Representing a number
  TokTrue,  // Representing a boolean true
  TokFalse, // Representing a boolean false
  TokChar,  // Representing a char literal
  TokTick,  // ' (linear pointer prefix)
  // TokIf
  TokIf,    // if
  TokElse,  // else
  TokWhile, // while

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
    {TokEnum, "enum"},
    {TokCase, "case"},
    {TokFatArrow, "=>"},
    {TokID, "identifier"},

    {TokNum, "number"},
    {TokChar, "char literal"},
    {TokTick, "'"},

    {TokIf, "if"},
    {TokElse, "else"},
    {TokWhile, "while"},

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
  using TokenList = std::list<std::shared_ptr<Token>>;
  using Iterator = TokenList::iterator;

  TokenList TokStream;
  Iterator cursor;
  Iterator rollback_cursor;
  bool error;
  std::function<void()> tokenProducer;

  struct SplitRecord {
    Iterator inserted;
    std::shared_ptr<Token> original;
  };
  std::vector<SplitRecord> pending_splits;
  size_t splits_at_mark = 0;

  void ensureToken() {
    while (cursor == TokStream.end() && tokenProducer)
      tokenProducer();
  }

public:
  std::vector<std::shared_ptr<Token>> ErrStream;

  TokenStream()
      : cursor(TokStream.end()), rollback_cursor(TokStream.end()),
        error(false) {}

  void setTokenProducer(std::function<void()> producer) {
    tokenProducer = std::move(producer);
  }

  void push_back(const std::shared_ptr<Token> &token) {
    if (token->tok_type == TokINVALID) {
      error = true;
      ErrStream.push_back(token);
      return;
    }
    bool was_at_end = (cursor == TokStream.end());
    TokStream.push_back(token);
    if (was_at_end)
      cursor = std::prev(TokStream.end());
  }

  bool hasErrors() const { return error; }

  void push_back(const Token &token) {
    this->push_back(std::make_shared<Token>(token));
  }

  void mark_rollback() {
    rollback_cursor = cursor;
    splits_at_mark = pending_splits.size();
  }

  void rollback() {
    while (pending_splits.size() > splits_at_mark) {
      auto &r = pending_splits.back();
      *std::prev(r.inserted) = r.original;
      TokStream.erase(r.inserted);
      pending_splits.pop_back();
    }
    cursor = rollback_cursor;
  }

  void rollback(size_t rollback_count) {
    for (size_t i = 0; i < rollback_count; ++i) {
      assert(cursor != TokStream.begin() &&
             "Cannot rollback past the beginning");
      --cursor;
    }
  }

  std::shared_ptr<Token> &exhaust_until(TokenType tokType) {
    if (tokType == TokenType::TokEOF) {
      while (tokenProducer) {
        if (!TokStream.empty() && TokStream.back()->tok_type == TokEOF)
          break;
        tokenProducer();
      }
      cursor = std::prev(TokStream.end());
      return TokStream.back();
    }
    while (!isEnd()) {
      ensureToken();
      if ((*cursor)->tok_type == tokType) {
        auto &ref = *cursor;
        ++cursor;
        return ref;
      } else {
        ++cursor;
      }
    }
    return TokStream.back();
  }

  bool isEnd() {
    ensureToken();
    return cursor != TokStream.end() && (*cursor)->tok_type == TokEOF;
  }

  std::shared_ptr<Token> peek() {
    ensureToken();
    return *cursor;
  }

  std::shared_ptr<Token> consume() {
    ensureToken();
    auto token = *cursor;
    if (token->tok_type != TokEOF)
      ++cursor;
    return token;
  }

  sammine_util::Location currentLocation() {
    ensureToken();
    if (!TokStream.empty())
      return (*cursor)->get_location();
    return {};
  }

  void split_current(TokenType first_type, const std::string &first_lex,
                     TokenType second_type, const std::string &second_lex) {
    ensureToken();
    auto original = *cursor;
    auto loc = original->location;
    *cursor = std::make_shared<Token>(first_type, first_lex, loc);
    auto inserted = TokStream.insert(
        std::next(cursor),
        std::make_shared<Token>(second_type, second_lex, loc));
    pending_splits.push_back({inserted, original});
  }
};
} // namespace sammine_lang
