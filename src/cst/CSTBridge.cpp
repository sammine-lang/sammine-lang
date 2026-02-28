#include "cst/CSTBridge.h"
#include <cassert>

namespace sammine_lang {
namespace cst {

std::string_view
CSTBridge::get_token_text(const std::shared_ptr<Token> &tok) const {
  // String and char literals have processed lexemes (quotes stripped,
  // escapes resolved). Extract raw source text using byte positions.
  if ((tok->tok_type == TokStr || tok->tok_type == TokChar) &&
      tok->location.source_start >= 0 &&
      tok->location.source_end > tok->location.source_start &&
      static_cast<size_t>(tok->location.source_end) <= source_text_.size()) {
    size_t start = static_cast<size_t>(tok->location.source_start);
    size_t len = static_cast<size_t>(tok->location.source_end -
                                     tok->location.source_start);
    return std::string_view(source_text_.data() + start, len);
  }
  return tok->lexeme;
}

void CSTBridge::emit_token(const std::shared_ptr<Token> &tok) {
  auto sk = from_token_type(tok->tok_type);
  auto text = get_token_text(tok);
  builder_.token(sk, text);
}

void CSTBridge::emit_leading_trivia() {
  while (raw_token_index_ < raw_tokens_.size()) {
    auto &raw = raw_tokens_[raw_token_index_];
    auto sk = from_token_type(raw->tok_type);
    if (!is_trivia(sk))
      break;
    emit_token(raw);
    raw_token_index_++;
  }
}

void CSTBridge::on_token_consumed(const std::shared_ptr<Token> &tok) {
  // Step 1: Handle second half of a previously split token
  if (split_remaining_kind_.has_value()) {
    builder_.token(*split_remaining_kind_, *split_remaining_text_);
    split_remaining_kind_.reset();
    split_remaining_text_.reset();
    return;
  }

  // Step 2: Emit leading trivia
  emit_leading_trivia();

  // Step 3: Advance past any skipped tokens (e.g. from exhaust_until)
  // and find the consumed token in raw_tokens_ by pointer identity
  while (raw_token_index_ < raw_tokens_.size()) {
    auto &raw = raw_tokens_[raw_token_index_];

    // Check for pointer identity match
    if (raw.get() == tok.get()) {
      // Direct match — emit and advance
      emit_token(raw);
      raw_token_index_++;
      return;
    }

    // Check for split token: raw has TokSHR but consumed TokGREATER
    // (or raw has TokGreaterEqual but consumed TokGREATER)
    if ((raw->tok_type == TokSHR && tok->tok_type == TokGREATER) ||
        (raw->tok_type == TokGreaterEqual && tok->tok_type == TokGREATER)) {
      // Emit the first half (the consumed >)
      builder_.token(from_token_type(TokGREATER), ">");

      if (raw->tok_type == TokSHR) {
        // Save second half: another >
        split_remaining_kind_ = from_token_type(TokGREATER);
        split_remaining_text_ = ">";
      } else {
        // TokGreaterEqual split into > and =
        split_remaining_kind_ = from_token_type(TokASSIGN);
        split_remaining_text_ = "=";
      }
      raw_token_index_++;
      return;
    }

    // Skip trivia that we haven't emitted yet
    auto sk = from_token_type(raw->tok_type);
    if (is_trivia(sk)) {
      emit_token(raw);
      raw_token_index_++;
      continue;
    }

    // Non-trivia token that doesn't match — this is a skipped token
    // (e.g. from exhaust_until). Emit it to preserve lossless CST.
    emit_token(raw);
    raw_token_index_++;
  }

  // If we get here, the token wasn't found in raw_tokens_.
  // This can happen with synthetic tokens from split_current().
  // Emit it directly.
  auto sk = from_token_type(tok->tok_type);
  builder_.token(sk, tok->lexeme);
}

Marker CSTBridge::start_node(SyntaxKind kind) {
  emit_leading_trivia();
  return builder_.start_node(kind);
}

void CSTBridge::finish_node() { builder_.finish_node(); }

void CSTBridge::abandon(Marker m) { builder_.abandon(m); }

Marker CSTBridge::start_node_at(BridgeCheckpoint cp, SyntaxKind kind) {
  return builder_.start_node_at(cp.tree_cp, kind);
}

CSTBridge::BridgeCheckpoint CSTBridge::checkpoint() const {
  return BridgeCheckpoint{builder_.checkpoint(), raw_token_index_};
}

void CSTBridge::rollback_to(BridgeCheckpoint cp) {
  builder_.rollback_to(cp.tree_cp);
  raw_token_index_ = cp.raw_index;
  split_remaining_kind_.reset();
  split_remaining_text_.reset();
}

void CSTBridge::finish_remaining() {
  // Emit any remaining trivia/tokens
  while (raw_token_index_ < raw_tokens_.size()) {
    auto &raw = raw_tokens_[raw_token_index_];
    if (raw->tok_type == TokEOF) {
      raw_token_index_++;
      continue;
    }
    emit_token(raw);
    raw_token_index_++;
  }
}

const GreenNode *CSTBridge::finish() { return builder_.finish(); }

} // namespace cst
} // namespace sammine_lang
