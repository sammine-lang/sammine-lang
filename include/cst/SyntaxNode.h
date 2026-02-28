#pragma once

#include "cst/GreenNode.h"
#include "cst/SyntaxKind.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sammine_lang {
namespace cst {

class SyntaxNode;

/// A child entry returned by SyntaxNode::children().
struct SyntaxChild {
  GreenElement green;
  uint32_t offset; // absolute offset from source start
};

/// Red tree node: a lightweight wrapper around a GreenNode with
/// parent pointers and absolute offsets. Created on-the-fly.
///
/// This is the user-facing API for navigating the CST.
/// 24 bytes: GreenNode* + parent* + uint32_t offset.
class SyntaxNode {
  const GreenNode *green_;
  const SyntaxNode *parent_;
  uint32_t offset_;

public:
  SyntaxNode(const GreenNode *green, const SyntaxNode *parent, uint32_t offset)
      : green_(green), parent_(parent), offset_(offset) {}

  /// The kind of this node
  SyntaxKind kind() const { return green_->kind(); }

  /// Total text length covered by this node
  uint32_t text_len() const { return green_->text_len(); }

  /// Absolute byte offset from the start of the source
  uint32_t offset() const { return offset_; }

  /// End offset (exclusive)
  uint32_t end_offset() const { return offset_ + text_len(); }

  /// Parent node (nullptr for root)
  const SyntaxNode *parent() const { return parent_; }

  /// Number of children
  uint32_t num_children() const { return green_->num_children(); }

  /// Get children with their absolute offsets
  std::vector<SyntaxChild> children() const;

  /// Find the first child node with the given kind
  std::optional<SyntaxNode> child_node(SyntaxKind kind) const;

  /// Find the text of the first child token with the given kind
  std::optional<std::string_view> child_token_text(SyntaxKind kind) const;

  /// Reconstruct the full source text covered by this node (lossless)
  std::string text() const;

  /// Find the deepest node containing the given byte offset
  SyntaxNode node_at_offset(uint32_t byte_offset) const;

  /// Access the underlying green node
  const GreenNode *green() const { return green_; }

  /// Print tree structure for debugging
  void dump(std::string &out, int indent = 0) const;
  std::string dump() const;
};

/// Owns the green tree infrastructure and provides the root SyntaxNode.
class SyntaxTree {
  std::unique_ptr<GreenInterner> interner_;
  const GreenNode *root_;

public:
  SyntaxTree(std::unique_ptr<GreenInterner> interner, const GreenNode *root)
      : interner_(std::move(interner)), root_(root) {}

  /// Get the root SyntaxNode (red tree entry point)
  SyntaxNode root() const { return SyntaxNode(root_, nullptr, 0); }

  /// Get the root green node
  const GreenNode *green_root() const { return root_; }
};

} // namespace cst
} // namespace sammine_lang
