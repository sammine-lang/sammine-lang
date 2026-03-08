//
// Created by jjasmine on 3/8/24.
//

#define DEBUG_TYPE "lexer"

#include "lex/Lexer.h"
#include "fmt/core.h"
#include "functional"
#include "lex/Token.h"
#include "util/Logging.h"
#include <cassert>
#include <string>

//! \file Lexer.cpp
//! \brief Implementation for scanning and streaming Tokens
namespace sammine_lang {

using LexFunction = std::function<size_t(Lexer *, size_t, const std::string &)>;

std::shared_ptr<Token> Lexer::peek() { return tokStream->peek(); }

std::shared_ptr<Token> Lexer::consume() { return tokStream->consume(); }

void Lexer::updateLocation() {
  auto si = location.source_info;
  location = sammine_util::Location(location.source_end, location.source_end);
  location.source_info = si;
}
Lexer::Lexer(const std::string &input) : Lexer() {
  LOG({
    fmt::print(stderr, "[{}] Starting lexical analysis on input of {} bytes\n",
               DEBUG_TYPE, input.size());
  });

  stored_input = input;
  cursor = 0;
  at_eof = false;
  tokStream->setTokenProducer([this]() { lexNextToken(); });
}

Lexer::Lexer(const std::string &input,
             std::shared_ptr<sammine_util::SourceInfo> source_info)
    : Lexer(input) {
  source_info_ = std::move(source_info);
  location.source_info = source_info_;
}

void Lexer::lexNextToken() {
  if (at_eof)
    return;
  if (cursor >= stored_input.length()) {
    tokStream->push_back({TokEOF, "end of file", location});
    at_eof = true;
    return;
  }
  cursor = handleSpaces(cursor, stored_input);
  if (cursor >= stored_input.length()) {
    tokStream->push_back({TokEOF, "end of file", location});
    at_eof = true;
    return;
  }
  updateLocation();
  static std::vector<LexFunction> MatchFunctions = {
      &Lexer::handleID,
      &Lexer::handleNumber,
      &Lexer::handleOperators,
      &Lexer::handleUtility,
  };
  size_t i_0 = cursor;
  for (auto fn : MatchFunctions) {
    i_0 = cursor;
    cursor = fn(this, cursor, stored_input);
    if (cursor != i_0) {
      updateLocation();
      return;
    }
  }
  if (cursor == i_0) {
    cursor = handleInvalid(cursor, stored_input);
  }
}

size_t Lexer::handleID(size_t i, const std::string &input) {
  if (input[i] == '\'') {
    // Disambiguate: 'ptr<T> (tick for linear pointer) vs 'a' (char literal)
    // If next char is alpha AND char after that is NOT ', it's a tick token
    if (i + 2 < input.length() && isalpha(input[i + 1]) && input[i + 2] != '\'') {
      i = advance(i); // consume the '
      tokStream->push_back(Token(TokTick, "'", location));
      return i; // leave the alpha char for the next token (e.g., "ptr")
    }
    i = advance(i);
    char ch;
    if (input[i] == '\\') {
      i = advance(i);
      switch (input[i]) {
      case 'n':
        ch = '\n';
        break;
      case 't':
        ch = '\t';
        break;
      case 'r':
        ch = '\r';
        break;
      case '\\':
        ch = '\\';
        break;
      case '\'':
        ch = '\'';
        break;
      case '0':
        ch = '\0';
        break;
      default:
        ch = input[i];
        break;
      }
    } else {
      ch = input[i];
    }
    i = advance(i);
    if (i < input.length() && input[i] == '\'') {
      i = advance(i);
      tokStream->push_back(Token(TokChar, std::string(1, ch), location));
    } else {
      tokStream->push_back(
          Token(TokINVALID, std::string(1, ch), location));
      add_error(location, "Unterminated char literal, expected closing '");
    }
    return i;
  }
  if (input[i] == '\"') {
    std::string str = "";
    i = advance(i);
    while (input[i] != '\"') {
      if (input[i] == '\\') {
        i = advance(i);
        switch (input[i]) {
        case 'n':
          str += '\n';
          break;
        case 't':
          str += '\t';
          break;
        case 'r':
          str += '\r';
          break;
        case '\\':
          str += '\\';
          break;
        case '\"':
          str += '\"';
          break;
        case '0':
          str += '\0';
          break;
        default:
          str += input[i];
          break;
        }
      } else {
        str += input[i];
      }
      i = advance(i);
    }
    i = advance(i);
    tokStream->push_back({TokStr, str, location});
    return i;
  }
  if (isalpha(input[i]) or
      (input[i] == '_')) { // identifier: [a-zA-Z_][a-zA-Z0-9_$]*
    std::string IdentifierStr;
    IdentifierStr = input[i];

    i = advance(i);
    while (isalnum(input[i]) or (input[i] == '_') or (input[i] == '$')) {
      IdentifierStr += input[i];
      i = advance(i);
    }

    static const std::unordered_map<std::string, TokenType> keywords = {
        {"fn", TokFunc},       {"return", TokReturn}, {"struct", TokStruct},
        {"type", TokType},     {"if", TokIf},         {"else", TokElse},
        {"let", TokLet},       {"mut", TokMUT},       {"true", TokTrue},
        {"false", TokFalse},   {"reuse", TokReuse},   {"export", TokExport},
        {"ptr", TokPtr},
        {"alloc", TokAlloc},   {"free", TokFree},     {"len", TokLen},
        {"dim", TokDim},
        {"import", TokImport}, {"as", TokAs},
        {"typeclass", TokTypeclass}, {"instance", TokInstance},
        {"while", TokWhile},
        {"case", TokCase},
        {"kernel", TokKernel},
    };
    auto it = keywords.find(IdentifierStr);
    tokStream->push_back(Token(it != keywords.end() ? it->second : TokID,
                               IdentifierStr, location));
  }

  return i;
}

size_t Lexer::handleNumber(size_t i, const std::string &input) {
  std::string NumStr = {};

  if (isdigit(input[i])) {
    do {
      NumStr += input[i];
      i = advance(i);
    } while (i < input.length() - 1 && isdigit(input[i]));

    if (input[i] == '.') {
      NumStr += input[i];
      i = advance(i);
      do {
        NumStr += input[i];
        i = advance(i);
      } while (i < input.length() - 1 && isdigit(input[i]));
    }

    // Consume any type suffix (e.g., i32, i64, f64)
    if (i < input.length() - 1 && isalpha(input[i])) {
      while (i < input.length() - 1 && isalnum(input[i])) {
        NumStr += input[i];
        i = advance(i);
      }
    }

    tokStream->push_back(Token(TokNum, NumStr, location));

  } else if (input[i] == '.') {
    // Check for ellipsis (...)
    if (i + 2 < input.length() && input[i + 1] == '.' && input[i + 2] == '.') {
      i = advance(i);
      i = advance(i);
      i = advance(i);
      tokStream->push_back(Token(TokEllipsis, "...", location));
      return i;
    }

    auto i_0 = i;
    NumStr += input[i];
    i = advance(i);

    while (i < input.length() - 1 && isdigit(input[i])) {
      NumStr += input[i];
      i = advance(i);
    }

    if (i - 1 == i_0) {
      tokStream->push_back(Token(TokDot, ".", location));
    } else {
      tokStream->push_back(Token(TokNum, NumStr, location));
    }
  }
  return i;
}

size_t Lexer::handleSpaces(size_t i, const std::string &input) {
  while (isspace(input[i])) {
    i = advance(i);
  }

  return i;
}

size_t Lexer::handleInvalid(size_t i, const std::string &input) {
  size_t start = i;
  i = advance(i);
  while (i < input.length() && !isspace(input[i]) && !isalpha(input[i]) &&
         !isdigit(input[i]) && input[i] != '_' && input[i] != '"' &&
         input[i] != '+' && input[i] != '-' && input[i] != '*' &&
         input[i] != '/' && input[i] != '%' && input[i] != '&' &&
         input[i] != '|' && input[i] != '^' && input[i] != '<' &&
         input[i] != '>' && input[i] != '=' && input[i] != '!' &&
         input[i] != '(' && input[i] != ')' && input[i] != '{' &&
         input[i] != '}' && input[i] != '[' && input[i] != ']' &&
         input[i] != '#' && input[i] != ',' && input[i] != ';' &&
         input[i] != ':' && input[i] != '.' && input[i] != '\'') {
    i = advance(i);
  }
  tokStream->push_back(
      Token(TokINVALID, input.substr(start, i - start), location));
  add_error(location, "Encountered invalid token");
  return i;
}

struct OpExtension {
  char second;
  TokenType tok;
  const char *lexeme;
};

struct OpRule {
  char lead;
  TokenType fallback_tok;
  const char *fallback_lexeme;
  OpExtension extensions[3];
};

// clang-format off
static const OpRule operatorRules[] = {
    {'+', TokADD, "+",        {{'+', TokAddIncr, "++"}, {'=', TokAddAssign, "+="}}},
    {'-', TokSUB, "-",        {{'-', TokSubDecr, "--"}, {'>', TokArrow, "->"}, {'=', TokSubAssign, "-="}}},
    {'*', TokMUL, "*",        {{'*', TokEXP, "**"}, {'=', TokMulAssign, "*="}}},
    {'/', TokDIV, "/",        {{'^', TokCeilDiv, "/^"}, {'_', TokFloorDiv, "/_"}, {'=', TokDivAssign, "/="}}},
    {'%', TokMOD, "%",        {}},
    {'&', TokAndLogical, "&", {{'&', TokAND, "&"}}},
    {'|', TokORLogical, "|",  {{'|', TokOR, "||"}, {'>', TokPipe, "|>"}}},
    {'^', TokXOR, "^",        {}},
    {'<', TokLESS, "<",       {{'<', TokSHL, "<<"}, {'=', TokLessEqual, "<="}}},
    {'>', TokGREATER, ">",    {{'>', TokSHR, ">>"}, {'=', TokGreaterEqual, ">="}}},
    {'=', TokASSIGN, "=",     {{'=', TokEQUAL, "=="}, {'>', TokFatArrow, "=>"}}},
    {'!', TokNOT, "!",        {{'=', TokNOTEqual, "!="}}},
};
// clang-format on

size_t Lexer::handleOperators(size_t i, const std::string &input) {
  for (const auto &rule : operatorRules) {
    if (input[i] != rule.lead)
      continue;

    if (i + 1 < input.length()) {
      for (const auto &ext : rule.extensions) {
        if (ext.second == '\0')
          break;
        if (input[i + 1] == ext.second) {
          i = advance(i);
          i = advance(i);
          tokStream->push_back(Token(ext.tok, ext.lexeme, location));
          updateLocation();
          return i;
        }
      }
    }

    i = advance(i);
    tokStream->push_back(Token(rule.fallback_tok, rule.fallback_lexeme, location));
    updateLocation();
    return i;
  }
  return i;
}

size_t Lexer::handleUtility(size_t i, const std::string &input) {
  size_t i_0 = 0;

  static std::vector<LexFunction> MatchFunctions = {
      &Lexer::handleUtilityPAREN,   &Lexer::handleUtilityCURLY,
      &Lexer::handleUtilityCOMMENT, &Lexer::handleUtilityCOMMA,
      &Lexer::handleUtilityCOLON,   &Lexer::handleUtilitySemiColon,
      &Lexer::handleUtilityBRACKET,
  };

  for (auto fn : MatchFunctions) {
    i_0 = i;
    i = fn(this, i, input);
    if (i != i_0) {
      updateLocation();
      break;
    }
  }

  return i;
}

size_t Lexer::handleUtilityPAREN(size_t i, const std::string &input) {
  if (input[i] == '(') {
    i = advance(i);
    tokStream->push_back(Token(TokLeftParen, "(", location));
    return i;
  }
  if (input[i] == ')') {
    i = advance(i);
    tokStream->push_back(Token(TokRightParen, ")", location));
    return i;
  }
  return i;
}

size_t Lexer::handleUtilityCURLY(size_t i, const std::string &input) {
  if (input[i] == '{') {
    i = advance(i);
    tokStream->push_back(Token(TokLeftCurly, "{", location));
    return i;
  }
  if (input[i] == '}') {
    i = advance(i);
    tokStream->push_back(Token(TokRightCurly, "}", location));
    return i;
  }
  return i;
}

size_t Lexer::handleUtilityCOMMENT(size_t i, const std::string &input) {
  if (input[i] == '#') {
    while (i < input.length() && input[i] != '\n') {
      i = advance(i);
    }
    i = advance(i);
  }
  return i;
}

size_t Lexer::handleUtilityCOMMA(size_t i, const std::string &input) {
  if (input[i] == ',') {
    i = advance(i);
    tokStream->push_back(Token(TokComma, ",", location));
  }
  return i;
}

size_t Lexer::handleUtilityCOLON(size_t i, const std::string &input) {
  if (input[i] == ':') {
    if (input.length() - 1 < i + 1 || (input[i + 1] != ':')) {
      i = advance(i);
      tokStream->push_back(Token(TokColon, ":", location));
      return i;
    }
    i = advance(i);

    if (input[i] == ':') {
      i = advance(i);
      tokStream->push_back(Token(TokDoubleColon, "::", location));
      return i;
    }

    i = devance(i);
  }
  return i;
}

size_t Lexer::handleUtilitySemiColon(size_t i, const std::string &input) {
  if (input[i] == ';') {
    i = advance(i);
    tokStream->push_back(Token(TokSemiColon, ";", location));
  }
  return i;
}

size_t Lexer::handleUtilityBRACKET(size_t i, const std::string &input) {
  if (input[i] == '[') {
    i = advance(i);
    tokStream->push_back(Token(TokLeftBracket, "[", location));
    return i;
  }
  if (input[i] == ']') {
    i = advance(i);
    tokStream->push_back(Token(TokRightBracket, "]", location));
    return i;
  }
  return i;
}

size_t Lexer::advance(size_t i) {
  i++;
  location.advance();
  return i;
}

size_t Lexer::devance(size_t i) {
  if (i > 0)
    i--;
  location.devance();
  return i;
}
} // namespace sammine_lang
