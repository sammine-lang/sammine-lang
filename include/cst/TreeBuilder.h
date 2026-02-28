#pragma once

#include "cst/GreenNode.h"
#include "cst/SyntaxKind.h"
#include <cassert>
#include <cstdint>
#include <string_view>
#include <vector>

namespace sammine_lang {
namespace cst {

/// Opaque handle to a started node (returned by start_node).
struct Marker {
  uint32_t index; // position in the element stack when the node started
};

/// Opaque handle to a checkpoint (for speculative parsing rollback).
struct CheckpointId {
  uint32_t element_count;
  uint32_t marker_count;
};

/// Marker-based builder that the parser calls to construct a green tree.
///
/// Usage:
///   builder.start_node(SyntaxKind::FuncDef);
///   builder.token(SyntaxKind::TokLet, "let");
///   builder.token(SyntaxKind::TokWhitespace, " ");
///   // ... recurse for children ...
///   builder.finish_node();  // creates GreenNode from accumulated children
///
/// Supports speculative parsing:
///   auto cp = builder.checkpoint();
///   // try parsing...
///   builder.rollback_to(cp);  // discard everything since checkpoint
class TreeBuilder {
  GreenInterner &interner_;

  struct StartedNode {
    SyntaxKind kind;
    uint32_t first_child; // index into elements_ where children start
  };

  /// Stack of elements being accumulated. When finish_node() is called,
  /// elements from the started node's first_child to the end become
  /// children of the new GreenNode, which replaces them as a single element.
  std::vector<GreenElement> elements_;

  /// Stack of started (but not yet finished) nodes.
  std::vector<StartedNode> node_stack_;

public:
  explicit TreeBuilder(GreenInterner &interner) : interner_(interner) {}

  /// Start a new node. All tokens/nodes added after this become children.
  Marker start_node(SyntaxKind kind);

  /// Finish the most recently started node, creating a GreenNode from
  /// all elements accumulated since start_node().
  void finish_node();

  /// Abandon a started node without creating a GreenNode.
  /// The children remain in the element stack (they become siblings
  /// of the abandoned node's parent).
  void abandon(Marker m);

  /// Add a token leaf to the current node.
  void token(SyntaxKind kind, std::string_view text);

  /// Save current state for speculative parsing.
  CheckpointId checkpoint() const;

  /// Rollback to a previous checkpoint, discarding all elements and
  /// started nodes accumulated since.
  void rollback_to(CheckpointId cp);

  /// Retroactively start a node at an earlier checkpoint position.
  /// All elements emitted since the checkpoint become children of the new node.
  /// Asserts that no open (unfinished) nodes exist between checkpoint and now.
  Marker start_node_at(CheckpointId cp, SyntaxKind kind);

  /// Finish building and return the root GreenNode.
  /// Asserts that exactly one element remains and no nodes are open.
  const GreenNode *finish();
};

} // namespace cst
} // namespace sammine_lang
