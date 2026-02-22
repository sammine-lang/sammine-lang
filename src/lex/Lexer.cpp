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
  location = sammine_util::Location(location.source_end, location.source_end);
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

    if (IdentifierStr == "fn")
      tokStream->push_back(Token(TokFunc, "fn", location));
    else if (IdentifierStr == "return")
      tokStream->push_back(Token(TokReturn, "return", location));
    else if (IdentifierStr == "Record")
      tokStream->push_back(Token(TokRecord, "Record", location));
    else if (IdentifierStr == "Type")
      tokStream->push_back(Token(TokType, "Type", location));
    else if (IdentifierStr == "if")
      tokStream->push_back(Token(TokIf, "if", location));
    else if (IdentifierStr == "else")
      tokStream->push_back(Token(TokElse, "else", location));
    else if (IdentifierStr == "let")
      tokStream->push_back(Token(TokLet, "let", location));
    else if (IdentifierStr == "mut")
      tokStream->push_back(Token(TokMUT, "mut", location));
    else if (IdentifierStr == "true")
      tokStream->push_back(Token(TokTrue, "true", location));
    else if (IdentifierStr == "false")
      tokStream->push_back(Token(TokFalse, "false", location));
    else if (IdentifierStr == "extern")
      tokStream->push_back(Token(TokExtern, "extern", location));
    else if (IdentifierStr == "ptr")
      tokStream->push_back(Token(TokPtr, "ptr", location));
    else if (IdentifierStr == "alloc")
      tokStream->push_back(Token(TokAlloc, "alloc", location));
    else if (IdentifierStr == "free")
      tokStream->push_back(Token(TokFree, "free", location));
    else if (IdentifierStr == "len")
      tokStream->push_back(Token(TokLen, "len", location));
    else if (IdentifierStr == "import")
      tokStream->push_back(Token(TokImport, "import", location));
    else if (IdentifierStr == "as")
      tokStream->push_back(Token(TokAs, "as", location));
    else
      tokStream->push_back(Token(TokID, IdentifierStr, location));
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
  i = advance(i);
  tokStream->push_back(Token(TokINVALID, input.substr(i - 1, 1), location));
  add_error(location, "Encountered invalid token");
  return i;
}

size_t Lexer::handleOperators(size_t i, const std::string &input) {
  size_t i_0 = 0;

  static std::vector<LexFunction> MatchFunctions = {
      &Lexer::handleOperatorsADD,      &Lexer::handleOperatorsSUB,
      &Lexer::handleOperatorsMUL,      &Lexer::handleOperatorsDIV,
      &Lexer::handleOperatorsMOD,      &Lexer::handleOperatorsAND,
      &Lexer::handleOperatorsOR,       &Lexer::handleOperatorsXOR,
      &Lexer::handleOperatorsCompLeft, &Lexer::handleOperatorsCompRight,
      &Lexer::handleOperatorsEqual,    &Lexer::handleOperatorsNot,
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

size_t Lexer::handleOperatorsADD(size_t i, const std::string &input) {
  if (input[i] == '+') {

    // If the next index (i+1) is outside of input, we should return ADD
    if (input.length() - 1 < i + 1 ||
        (input[i + 1] != '+' && input[i + 1] != '=')) {
      i = advance(i);
      tokStream->push_back(Token(TokADD, "+", location));
      return i;
    }

    i = advance(i);

    if (input[i] == '+') {
      i = advance(i);
      tokStream->push_back(Token(TokAddIncr, "++", location));
      return i;
    }

    if (input[i] == '=') {
      i = advance(i);
      tokStream->push_back(Token(TokAddAssign, "+=", location));
      return i;
    }

    i = devance(i);
  }
  return i;
}

size_t Lexer::handleOperatorsSUB(size_t i, const std::string &input) {
  if (input[i] == '-') {

    // If the next index (i+1) is outside of input, we should return ADD
    if (input.length() - 1 < i + 1 ||
        (input[i + 1] != '-' && input[i + 1] != '=' && input[i + 1] != '>')) {
      i = advance(i);
      tokStream->push_back(Token(TokSUB, "-", location));
      return i;
    }

    i = advance(i);

    if (input[i] == '-') {
      i = advance(i);
      tokStream->push_back(Token(TokSubDecr, "--", location));
      return i;
    }

    if (input[i] == '>') {
      i = advance(i);
      tokStream->push_back(Token(TokArrow, "->", location));
      return i;
    }

    if (input[i] == '=') {
      i = advance(i);
      tokStream->push_back(Token(TokSubAssign, "-=", location));
      return i;
    }

    i = devance(i);
  }
  return i;
}

size_t Lexer::handleOperatorsMUL(size_t i, const std::string &input) {
  if (input[i] == '*') {

    // If the next index (i+1) is outside of input, we should return ADD
    if (input.length() - 1 < i + 1 ||
        (input[i + 1] != '*' && input[i + 1] != '=')) {
      i = advance(i);
      tokStream->push_back(Token(TokMUL, "*", location));
      return i;
    }

    i = advance(i);

    if (input[i] == '*') {
      i = advance(i);
      tokStream->push_back(Token(TokEXP, "**", location));
      return i;
    }

    if (input[i] == '=') {
      i = advance(i);
      tokStream->push_back(Token(TokMulAssign, "*=", location));
      return i;
    }

    i = devance(i);
  }
  return i;
}

size_t Lexer::handleOperatorsDIV(size_t i, const std::string &input) {
  if (input[i] == '/') {

    // If the next index (i+1) is outside of input, we should return ADD
    if (input.length() - 1 < i + 1 ||
        (input[i + 1] != '^' && input[i + 1] != '_' && input[i + 1] != '=')) {
      i = advance(i);
      tokStream->push_back(Token(TokDIV, "/", location));
      return i;
    }

    i = advance(i);

    if (input[i] == '^') {
      i = advance(i);
      tokStream->push_back(Token(TokCeilDiv, "/^", location));
      return i;
    }

    if (input[i] == '_') {
      i = advance(i);
      tokStream->push_back(Token(TokFloorDiv, "/_", location));
      return i;
    }

    if (input[i] == '=') {
      i = advance(i);
      tokStream->push_back(Token(TokDivAssign, "/=", location));
      return i;
    }

    i = devance(i);
  }
  return i;
}

size_t Lexer::handleOperatorsMOD(size_t i, const std::string &input) {
  if (input[i] == '%') {
    i = advance(i);
    tokStream->push_back(Token(TokMOD, "%", location));
  }
  return i;
}

size_t Lexer::handleOperatorsAND(size_t i, const std::string &input) {
  if (input[i] == '&') {

    // If the next index (i+1) is outside of input, we should return ADD
    if (input.length() - 1 < i + 1 || (input[i + 1] != '&')) {
      i = advance(i);
      tokStream->push_back(Token(TokAndLogical, "&", location));
      return i;
    }

    i = advance(i);

    if (input[i] == '&') {
      i = advance(i);
      tokStream->push_back(Token(TokAND, "&", location));
      return i;
    }

    i = devance(i);
  }
  return i;
}

size_t Lexer::handleOperatorsOR(size_t i, const std::string &input) {
  if (input[i] == '|') {

    // If the next index (i+1) is outside of input, we should return logical OR
    if (input.length() - 1 < i + 1) {
      i = advance(i);
      tokStream->push_back(Token(TokORLogical, "|", location));
      return i;
    }

    if (input[i + 1] == '>') {
      i = advance(i);
      i = advance(i);
      tokStream->push_back(Token(TokPipe, "|>", location));
      return i;
    }

    if (input[i + 1] == '|') {
      i = advance(i);
      i = advance(i);
      tokStream->push_back(Token(TokOR, "||", location));
      return i;
    }

    i = advance(i);
    tokStream->push_back(Token(TokORLogical, "|", location));
  }
  return i;
}

size_t Lexer::handleOperatorsXOR(size_t i, const std::string &input) {
  if (input[i] == '^') {
    i = advance(i);
    tokStream->push_back(Token(TokXOR, "^", location));
  }
  return i;
}

size_t Lexer::handleOperatorsCompLeft(size_t i, const std::string &input) {
  if (input[i] == '<') {

    // If the next index (i+1) is outside of input, we should return ADD
    if (input.length() - 1 < i + 1 ||
        (input[i + 1] != '<' && input[i + 1] != '=')) {
      i = advance(i);
      tokStream->push_back(Token(TokLESS, "<", location));
      return i;
    }

    i = advance(i);

    if (input[i] == '<') {
      i = advance(i);
      tokStream->push_back(Token(TokSHL, "<<", location));
      return i;
    }

    if (input[i] == '=') {
      i = advance(i);
      tokStream->push_back(Token(TokLessEqual, "<=", location));
      return i;
    }

    i = devance(i);
  }
  return i;
}

size_t Lexer::handleOperatorsCompRight(size_t i, const std::string &input) {
  if (input[i] == '>') {

    // If the next index (i+1) is outside of input, we should return ADD
    if (input.length() - 1 < i + 1 ||
        (input[i + 1] != '>' && input[i + 1] != '=')) {
      i = advance(i);
      tokStream->push_back(Token(TokGREATER, ">", location));
      return i;
    }

    i = advance(i);

    if (input[i] == '>') {
      i = advance(i);
      tokStream->push_back(Token(TokSHR, ">>", location));
      return i;
    }

    if (input[i] == '=') {
      i = advance(i);
      tokStream->push_back(Token(TokGreaterEqual, ">=", location));
      return i;
    }

    i = devance(i);
  }
  return i;
}

size_t Lexer::handleOperatorsEqual(size_t i, const std::string &input) {
  if (input[i] == '=') {

    // If the next index (i+1) is outside of input, we should return ADD
    if (input.length() - 1 < i + 1 || (input[i + 1] != '=')) {
      i = advance(i);
      tokStream->push_back(Token(TokASSIGN, "=", location));
      return i;
    }

    i = advance(i);
    if (input[i] == '=') {
      i = advance(i);
      tokStream->push_back(Token(TokEQUAL, "==", location));
      return i;
    }

    i = devance(i);
  }
  return i;
}

size_t Lexer::handleOperatorsNot(size_t i, const std::string &input) {
  if (input[i] == '!') {
    if (input.length() - 1 >= i + 1 && input[i + 1] == '=') {
      i = advance(i);
      i = advance(i);
      tokStream->push_back(Token(TokNOTEqual, "!=", location));
      return i;
    }
    i = advance(i);
    tokStream->push_back(Token(TokNOT, "!", location));
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
