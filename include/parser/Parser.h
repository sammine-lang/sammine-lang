#pragma once
#include "ast/Ast.h"
#include "ast/AstDecl.h"
#include "lex/Token.h"
#include "util/Utilities.h"
#include <memory>
#include <optional>
#include <utility>
//! \file Parser.h
//! \brief Defines Parser, which consumes tokens and constructs the AST

namespace sammine_lang {
enum ParserError {
  SUCCESS,
  COMMITTED_NO_MORE_ERROR,
  COMMITTED_EMIT_MORE_ERROR,
  NONCOMMITTED,
};
using namespace AST;
using namespace sammine_util;
class Parser : public Reportee {

  void error(const std::string &msg, Location loc = Location::NonPrintable()) {
    if (reporter.has_value()) {
      reporter->get().immediate_error(msg, loc);
    }
    this->error_count++;
  }
  void diag(const std::string &msg, Location loc = Location::NonPrintable()) {
    if (reporter.has_value()) {
      reporter->get().immediate_diag(msg, loc);
    }
  }

public:
  template <class T> using p = std::pair<std::unique_ptr<T>, ParserError>;
  template <class T> using u = std::unique_ptr<T>;
  std::optional<std::reference_wrapper<Reporter>> reporter;
  std::shared_ptr<TokenStream> tokStream;
  [[nodiscard]] auto ParseProgram() -> u<ProgramAST>;

  // Parse definition
  [[nodiscard]] auto ParseDefinition() -> p<DefinitionAST>;
  [[nodiscard]] auto ParsePrototype() -> p<PrototypeAST>;
  [[nodiscard]] auto ParseFuncDef() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseVarDef() -> p<ExprAST>;
  [[nodiscard]] auto ParseTypeDef() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseRecordDef() -> p<DefinitionAST>;

  // Parse type
  [[nodiscard]] auto ParseTypeExpr() -> std::unique_ptr<TypeExprAST>;
  [[nodiscard]] auto ParseTypedVar() -> p<TypedVarAST>;

  // Parse pressions
  [[nodiscard]] auto ParseExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParsePrimaryExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseBinaryExpr(int prededence, u<ExprAST> LHS)
      -> p<ExprAST>;
  [[nodiscard]] auto ParseBoolExpr() -> p<ExprAST>;

  [[nodiscard]] auto ParseDerefExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseAddrOfExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseAllocExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseFreeExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseCallExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseReturnExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseArguments()
      -> std::pair<std::vector<u<ExprAST>>, ParserError>;
  [[nodiscard]] auto ParseParenExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseIfExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseNumberExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseStringExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseVariableExpr() -> p<ExprAST>;

  // Parse block
  [[nodiscard]] auto ParseBlock() -> p<BlockAST>;

  // Parse parameters
  [[nodiscard]] auto ParseParams()
      -> std::pair<std::vector<u<TypedVarAST>>, ParserError>;

  // Utilities
  [[nodiscard]] auto expect(TokenType tokType, bool exhausts = false,
                            TokenType until = TokenType::TokEOF,
                            const std::string &message = "")
      -> std::shared_ptr<Token>;

  [[nodiscard]] Parser(
      std::optional<std::reference_wrapper<Reporter>> reporter = std::nullopt)
      : reporter(reporter) {}
  [[nodiscard]] Parser(
      std::shared_ptr<TokenStream> tokStream,
      std::optional<std::reference_wrapper<Reporter>> reporter = std::nullopt)
      : reporter(reporter), tokStream(tokStream) {}

  [[nodiscard]] auto Parse() -> u<ProgramAST>;
};
} // namespace sammine_lang
