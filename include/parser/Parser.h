#pragma once
#include "ast/Ast.h"
#include "ast/AstDecl.h"
#include "lex/Token.h"
#include "util/Utilities.h"
#include <map>
#include <memory>
#include <optional>
#include <utility>
//! \file Parser.h
//! \brief Defines Parser, which consumes tokens and constructs the AST

namespace sammine_lang {

struct ParsedQualifiedName {
  sammine_util::QualifiedName qn;
  sammine_util::Location location;
};

enum ParserError {
  SUCCESS,
  FAILED,
  NONCOMMITTED,
};

template <typename T> struct ParseResult {
  std::unique_ptr<T> node;
  ParserError status;
  bool ok() const { return status == SUCCESS; }
  bool failed() const { return status == FAILED; }
  bool uncommitted() const { return status == NONCOMMITTED; }
};
#define PARSER_UNREACHABLE()                                                   \
  do {                                                                         \
    sammine_util::abort("Unreachable");                                        \
    std::unreachable();                                                        \
  } while (0)
#define REQUIRE(var, tokType, msg, loc, ...)                                   \
  auto var = expect(tokType);                                                  \
  if (!(var)) {                                                                \
    imm_error(msg, loc);                                                       \
    return __VA_ARGS__;                                                        \
  }
using namespace AST;
using namespace sammine_util;
class Parser : public Reportee {
  Location last_exhaustible_loc = Location::NonPrintable();
  void imm_error(const std::string &msg,
                 Location loc = Location::NonPrintable(),
                 std::source_location src = std::source_location::current()) {
    if (loc == Location::NonPrintable())
      loc = last_exhaustible_loc;
    if (reporter.has_value()) {
      reporter->get().immediate_error(msg, loc, src);
    }
    this->error_count++;
  }
  void imm_diag(const std::string &msg,
                Location loc = Location::NonPrintable(),
                std::source_location src = std::source_location::current()) {
    if (loc == Location::NonPrintable())
      loc = last_exhaustible_loc;
    if (reporter.has_value()) {
      reporter->get().immediate_diag(msg, loc, src);
    }
  }
  void imm_warn(const std::string &msg,
                Location loc = Location::NonPrintable(),
                std::source_location src = std::source_location::current()) {
    if (loc == Location::NonPrintable())
      loc = last_exhaustible_loc;
    if (reporter.has_value()) {
      reporter->get().immediate_warn(msg, loc, src);
    }
  }
  void emit_if_uncommitted(
      ParserError result, const std::string &msg, Location loc,
      std::source_location src = std::source_location::current()) {
    if (result == NONCOMMITTED)
      imm_error(msg, loc, src);
  }

public:
  template <class T> using p = ParseResult<T>;
  template <class T> using u = std::unique_ptr<T>;
  template <class T> using ListResult = std::pair<std::vector<u<T>>, ParserError>;

private:
  template <typename NodeT>
  [[nodiscard]] auto ParseBuiltinCallExpr(TokenType keyword,
                                          const std::string &name) -> p<ExprAST>;

  template <typename NodeT>
  [[nodiscard]] auto ParseUnaryPrefixExpr(TokenType opTok,
                                          const std::string &opStr) -> p<ExprAST>;

  template <typename T, typename... Fns>
  auto tryParsers(Fns... fns) -> p<T> {
    p<T> result{nullptr, NONCOMMITTED};
    (void)((result = (this->*fns)(), result.status != NONCOMMITTED) || ...);
    return result;
  }

public:
  std::optional<std::reference_wrapper<Reporter>> reporter;
  std::shared_ptr<TokenStream> tokStream;
  std::map<std::string, std::string> alias_to_module;
  [[nodiscard]] auto ParseImport() -> std::optional<AST::ImportDecl>;
  [[nodiscard]] auto ParseProgram() -> u<ProgramAST>;

  // Parse definition
  [[nodiscard]] auto ParseDefinition() -> p<DefinitionAST>;
  [[nodiscard]] auto ParsePrototype() -> p<PrototypeAST>;
  [[nodiscard]] auto ParseFuncDef() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseVarDef() -> p<ExprAST>;
  [[nodiscard]] auto ParseStructDef() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseEnumDef() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseTypeClassDecl() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseTypeClassInstance() -> p<DefinitionAST>;

  // Parse type
  [[nodiscard]] auto ParseTypeExprTopLevel() -> std::unique_ptr<TypeExprAST>;
  [[nodiscard]] auto ParseTypeExpr() -> std::unique_ptr<TypeExprAST>;
  [[nodiscard]] auto ParseTypedVar() -> p<TypedVarAST>;

  // Parse expressions
  [[nodiscard]] auto ParseExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParsePrimaryExpr() -> p<ExprAST>;
  [[nodiscard]] auto parsePostfixOps(u<ExprAST> expr) -> p<ExprAST>;
  [[nodiscard]] auto ParseBinaryExpr(int prededence, u<ExprAST> LHS)
      -> p<ExprAST>;
  [[nodiscard]] auto ParseBoolExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseCharExpr() -> p<ExprAST>;

  [[nodiscard]] auto ParseUnaryNegExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseDerefExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseAddrOfExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseAllocExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseFreeExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseLenExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseArrayLiteralExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseCallExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseStructLiteralExpr(sammine_util::QualifiedName qn,
                                            Location qn_loc) -> p<ExprAST>;
  [[nodiscard]] auto ParseReturnExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseArguments() -> ListResult<ExprAST>;
  [[nodiscard]] auto ParseParenExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseIfExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseCaseExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseWhileExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseNumberExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseStringExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseVariableExpr() -> p<ExprAST>;

  // Parse block
  [[nodiscard]] auto ParseBlock() -> p<BlockAST>;

  // Parse parameters
  [[nodiscard]] auto ParseParams() -> ListResult<TypedVarAST>;

  // Utilities
  [[nodiscard]] auto expect(TokenType tokType, bool exhausts = false,
                            TokenType until = TokenType::TokEOF,
                            const std::string &message = "")
      -> std::shared_ptr<Token>;

  /// Greedily consume ::ID pairs after an already-consumed first TokID.
  /// \param first_tok     The already-consumed TokID token
  /// \param resolve_alias If true, resolve first segment through alias_to_module
  [[nodiscard]] auto parseQualifiedNameTail(std::shared_ptr<Token> first_tok,
                                            bool resolve_alias = true)
      -> ParsedQualifiedName;

  /// Speculatively parse <TypeExpr, ...> after a qualified name.
  /// On success, populates type_args and may update qn/qn_loc if ::member follows.
  /// On failure, rolls back the token stream and returns empty.
  [[nodiscard]] auto
  parseExplicitTypeArgsTail(sammine_util::QualifiedName &qn,
                            sammine_util::Location &qn_loc)
      -> std::vector<std::unique_ptr<TypeExprAST>>;

  [[nodiscard]] auto consumeClosingAngleBracket() -> bool;

  bool parsed_var_arg = false;
  int pending_deref = 0;
  std::shared_ptr<Token> pending_deref_tok;

  [[nodiscard]] Parser(
      std::optional<std::reference_wrapper<Reporter>> reporter = std::nullopt, const std::string &default_namespace = "")
      : reporter(reporter) {}
  [[nodiscard]] Parser(
      std::shared_ptr<TokenStream> tokStream,
      std::optional<std::reference_wrapper<Reporter>> reporter = std::nullopt, const std::string &default_namespace = "")
      : reporter(reporter), tokStream(tokStream) {}

  [[nodiscard]] auto Parse() -> u<ProgramAST>;
};
} // namespace sammine_lang
