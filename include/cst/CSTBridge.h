#pragma once

#include "cst/GreenNode.h"
#include "cst/SyntaxKind.h"
#include "cst/SyntaxNode.h"
#include "cst/TreeBuilder.h"
#include "lex/Token.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sammine_lang {
namespace cst {

/// Adapter between the parser and TreeBuilder that handles:
/// - Trivia injection: emits whitespace/newline/comment tokens from raw_tokens_
/// - Node lifecycle: start_node, finish_node, start_node_at
/// - Checkpoint/rollback synced between TreeBuilder and raw_token_index_
/// - Split token handling (>> split into > + > for nested generics)
class CSTBridge {
  TreeBuilder &builder_;
  const std::vector<std::shared_ptr<Token>> &raw_tokens_;
  const std::string &source_text_;
  size_t raw_token_index_ = 0;

  // For split token handling: when >> is split into > + >,
  // we emit the first > and save info about the second >
  std::optional<SyntaxKind> split_remaining_kind_;
  std::optional<std::string> split_remaining_text_;

public:
  struct BridgeCheckpoint {
    CheckpointId tree_cp;
    size_t raw_index;
  };

  CSTBridge(TreeBuilder &builder,
            const std::vector<std::shared_ptr<Token>> &raw_tokens,
            const std::string &source_text)
      : builder_(builder), raw_tokens_(raw_tokens), source_text_(source_text) {}

  /// Emit all trivia tokens from raw_tokens_ up to the next non-trivia token.
  void emit_leading_trivia();

  /// Called when the parser consumes a token. Handles trivia injection,
  /// skipped token catch-up, and split token management.
  void on_token_consumed(const std::shared_ptr<Token> &tok);

  /// Start a new CST node.
  Marker start_node(SyntaxKind kind);

  /// Finish the most recently started CST node.
  void finish_node();

  /// Abandon a started node.
  void abandon(Marker m);

  /// Retroactively start a node at an earlier checkpoint position.
  Marker start_node_at(BridgeCheckpoint cp, SyntaxKind kind);

  /// Save current state for speculative parsing.
  BridgeCheckpoint checkpoint() const;

  /// Rollback to a previous checkpoint.
  void rollback_to(BridgeCheckpoint cp);

  /// Emit any trailing trivia and ensure all tokens are consumed.
  void finish_remaining();

  /// Finish building and return the root GreenNode.
  const GreenNode *finish();

private:
  /// Get the raw source text for a token (handles string/char literals).
  std::string_view get_token_text(const std::shared_ptr<Token> &tok) const;

  /// Emit a single token to the TreeBuilder.
  void emit_token(const std::shared_ptr<Token> &tok);
};

} // namespace cst
} // namespace sammine_lang
