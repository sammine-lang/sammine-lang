#pragma once
#include "parser/Parser.h"
#include <utility>
#include <vector>

//! \file Combinators.h
//! \brief Reusable parser combinator utilities.

namespace sammine_lang {

/// Parse a comma-separated list where the element parser returns a nullable
/// unique_ptr (e.g. ParseTypeExpr). Returns empty vector if first element
/// fails. Returns partial results + false if a subsequent element fails.
template <typename T, typename F>
auto comma_separated(F element_parser, Parser &ctx)
    -> std::pair<std::vector<std::unique_ptr<T>>, bool> {
  std::vector<std::unique_ptr<T>> result;
  auto first = element_parser();
  if (!first)
    return {std::move(result), false};
  result.push_back(std::move(first));
  while (ctx.tokStream->peek()->tok_type == TokComma) {
    ctx.tokStream->consume();
    auto next = element_parser();
    if (!next)
      return {std::move(result), false};
    result.push_back(std::move(next));
  }
  return {std::move(result), true};
}

/// Parse items until a stop token, collecting results into a vector.
/// The element parser must return ParseResult<T>. Stops when:
///  - the stop token is seen (not consumed)
///  - the element parser returns non-SUCCESS
///  - the stream ends
///
///
/// Example: use this when parsing type class decl
///   typeclass Hash<T> {
///     Hash(val: T) -> u64; <- ParseFn parse = parsePrototype()
///   } <- TokenType stop = TokRightCurly
template <typename T, typename ParseFn>
auto collect_until(TokenType stop, Parser &ctx, ParseFn parse)
    -> std::vector<std::unique_ptr<T>> {
  std::vector<std::unique_ptr<T>> items;
  while (!ctx.tokStream->isEnd() &&
         ctx.tokStream->peek()->tok_type != stop) {
    auto [item, result] = parse();
    if (result != SUCCESS)
      break;
    items.push_back(std::move(item));
  }
  return items;
}

/// Parse a comma-separated list with error recovery, using ParseResult<T>.
/// Recovery protocol per element:
///  - SUCCESS:      push element, continue
///  - NONCOMMITTED: stop collecting (break)
///  - FAILED:       push partial element, exhaust to next comma, mark error
/// Returns: {items, had_error}
template <typename T, typename ParseFn>
auto comma_sep_recover(Parser &ctx, ParseFn parse)
    -> std::pair<std::vector<std::unique_ptr<T>>, bool> {
  std::vector<std::unique_ptr<T>> vec;
  bool error = false;

  auto handle = [&](auto &item, ParserError result) -> bool {
    if (result == SUCCESS) {
      vec.push_back(std::move(item));
      return true;
    }
    if (result == NONCOMMITTED)
      return false;
    // FAILED: push partial, exhaust to next comma
    if (item)
      vec.push_back(std::move(item));
    (void)ctx.expect(TokComma, true, TokComma);
    error = true;
    return true;
  };

  auto [first, first_result] = parse();
  if (!handle(first, first_result))
    return {std::move(vec), false};

  while (!ctx.tokStream->isEnd()) {
    if (!ctx.expect(TokComma))
      break;
    auto [elem, result] = parse();
    if (!handle(elem, result))
      break;
  }

  return {std::move(vec), error};
}

} // namespace sammine_lang
