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
  void imm_diag(const std::string &msg, Location loc = Location::NonPrintable(),
                std::source_location src = std::source_location::current()) {
    if (loc == Location::NonPrintable())
      loc = last_exhaustible_loc;
    if (reporter.has_value()) {
      reporter->get().immediate_diag(msg, loc, src);
    }
  }
  void imm_warn(const std::string &msg, Location loc = Location::NonPrintable(),
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

  /// expect() + imm_error() in one call. Returns the token on success,
  /// nullptr (with error reported) on failure.
  auto expectOrError(TokenType tokType, const std::string &msg, Location loc,
                     std::source_location src =
                         std::source_location::current())
      -> std::shared_ptr<Token> {
    auto tok = expect(tokType);
    if (!tok)
      imm_error(msg, loc, src);
    return tok;
  }

public:
  template <class T> using p = ParseResult<T>;
  template <class T> using u = std::unique_ptr<T>;
  template <class T>
  using ListResult = std::pair<std::vector<u<T>>, ParserError>;

private:
  template <typename NodeT>
  [[nodiscard]] auto ParseBuiltinCallExpr(TokenType keyword,
                                          const std::string &name)
      -> p<ExprAST>;

  template <typename NodeT>
  [[nodiscard]] auto ParseUnaryPrefixExpr(TokenType opTok,
                                          const std::string &opStr)
      -> p<ExprAST>;

  /// Try each parser in order until one commits (returns non-NONCOMMITTED).
  /// Accepts member function pointers and any callable taking no args.
  template <typename T, typename... Fns> auto tryParsers(Fns... fns) -> p<T> {
    p<T> result{nullptr, NONCOMMITTED};
    (void)((result = invokeParseFn<T>(fns),
            result.status != NONCOMMITTED) || ...);
    return result;
  }

  /// Dispatch helper: member function pointer → call on this
  template <typename T, typename R>
  auto invokeParseFn(R (Parser::*fn)()) -> p<T> {
    return (this->*fn)();
  }
  /// Dispatch helper: callable (lambda, etc.) → call directly
  template <typename T, typename F>
    requires(!std::is_member_pointer_v<F>)
  auto invokeParseFn(F &fn) -> p<T> {
    return fn();
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
  [[nodiscard]] auto ParsePrototypeWithSemi(const std::string &semi_msg)
      -> p<PrototypeAST>;
  [[nodiscard]] auto ParseFuncDef() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseReuseDef() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseVarDef() -> p<ExprAST>;
  [[nodiscard]] auto ParseStructDef() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseEnumDef() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseTypeClassDecl() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseTypeClassInstance() -> p<DefinitionAST>;
  [[nodiscard]] auto ParseKernelDef() -> p<DefinitionAST>;

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

  [[nodiscard]] auto ParseDerefExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseAllocExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseArrayLiteralExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseIdentifierExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseStructLiteralExpr(
      sammine_util::QualifiedName qn, Location qn_loc,
      std::vector<std::unique_ptr<TypeExprAST>> type_args = {}) -> p<ExprAST>;
  [[nodiscard]] auto ParseReturnStmt() -> p<ExprAST>;
  [[nodiscard]] auto ParseArguments() -> ListResult<ExprAST>;
  [[nodiscard]] auto ParseParenExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseIfExpr() -> p<ExprAST>;
  [[nodiscard]] auto ParseCaseExpr() -> p<ExprAST>;
  [[nodiscard]] auto parseCasePattern() -> std::optional<CasePattern>;
  [[nodiscard]] auto ParseWhileExpr() -> p<ExprAST>;

  // Parse block
  [[nodiscard]] auto ParseBlock() -> p<BlockAST>;

  // Parse parameters
  struct ParamsResult {
    std::vector<u<TypedVarAST>> params;
    ParserError status;
    bool is_var_arg = false;
  };
  [[nodiscard]] auto ParseParams() -> ParamsResult;

  // Utilities
  [[nodiscard]] auto expect(TokenType tokType, bool exhausts = false,
                            TokenType until = TokenType::TokEOF,
                            const std::string &message = "")
      -> std::shared_ptr<Token>;

  /// Greedily consume ::ID pairs after an already-consumed first TokID.
  /// \param first_tok     The already-consumed TokID token
  /// \param resolve_alias If true, resolve first segment through
  /// alias_to_module
  [[nodiscard]] auto parseQualifiedNameTail(std::shared_ptr<Token> first_tok,
                                            bool resolve_alias = true)
      -> ParsedQualifiedName;

  /// Speculatively parse <TypeExpr, ...> after a qualified name.
  /// On success, populates type_args and may update qn/qn_loc if ::member
  /// follows. On failure, rolls back the token stream and returns empty.
  [[nodiscard]] auto parseExplicitTypeArgsTail(sammine_util::QualifiedName &qn,
                                               sammine_util::Location &qn_loc)
      -> std::vector<std::unique_ptr<TypeExprAST>>;

  [[nodiscard]] auto consumeClosingAngleBracket() -> bool;

  /// Parse <T, U, ...> type parameter list. Returns empty if no '<' found.
  /// On parse error, returns the params parsed so far and sets \p had_error.
  [[nodiscard]] auto parseTypeParams(Location err_loc)
      -> std::pair<std::vector<std::string>, bool>;

  int pending_deref = 0;
  std::shared_ptr<Token> pending_deref_tok;

  [[nodiscard]] Parser(
      std::optional<std::reference_wrapper<Reporter>> reporter_ = std::nullopt,
      const std::string &default_namespace = "")
      : reporter(reporter_) {}
  [[nodiscard]] Parser(
      std::shared_ptr<TokenStream> tokStream_,
      std::optional<std::reference_wrapper<Reporter>> reporter_ = std::nullopt,
      const std::string &default_namespace = "")
      : reporter(reporter_), tokStream(tokStream_) {}

  [[nodiscard]] auto Parse() -> u<ProgramAST>;
};
} // namespace sammine_lang
