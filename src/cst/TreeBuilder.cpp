#include "cst/TreeBuilder.h"
#include <cassert>

namespace sammine_lang {
namespace cst {

Marker TreeBuilder::start_node(SyntaxKind kind) {
  uint32_t idx = static_cast<uint32_t>(elements_.size());
  node_stack_.push_back({kind, idx});
  return Marker{idx};
}

void TreeBuilder::finish_node() {
  assert(!node_stack_.empty() && "finish_node() with no open node");
  auto started = node_stack_.back();
  node_stack_.pop_back();

  // Collect children from elements_[started.first_child .. end)
  uint32_t first = started.first_child;

  std::vector<GreenElement> children(elements_.begin() + first,
                                     elements_.end());

  // Remove children from the element stack
  elements_.resize(first);

  // Create the green node and push it as a single element
  const GreenNode *node = interner_.node(started.kind, children);
  elements_.emplace_back(node);
}

void TreeBuilder::abandon(Marker m) {
  // Find and remove the matching started node from the stack.
  // The children remain as elements (they become siblings of the parent).
  assert(!node_stack_.empty() && "abandon() with no open node");
  assert(node_stack_.back().first_child == m.index &&
         "abandon() must match the most recently started node");
  node_stack_.pop_back();
}

void TreeBuilder::token(SyntaxKind kind, std::string_view text) {
  const GreenToken *tok = interner_.token(kind, text);
  elements_.emplace_back(tok);
}

CheckpointId TreeBuilder::checkpoint() const {
  return CheckpointId{static_cast<uint32_t>(elements_.size()),
                      static_cast<uint32_t>(node_stack_.size())};
}

void TreeBuilder::rollback_to(CheckpointId cp) {
  assert(cp.element_count <= elements_.size());
  assert(cp.marker_count <= node_stack_.size());
  elements_.resize(cp.element_count);
  node_stack_.resize(cp.marker_count);
}

Marker TreeBuilder::start_node_at(CheckpointId cp, SyntaxKind kind) {
  assert(cp.marker_count == node_stack_.size() &&
         "start_node_at: open nodes exist between checkpoint and now");
  assert(cp.element_count <= elements_.size());
  node_stack_.push_back({kind, cp.element_count});
  return Marker{cp.element_count};
}

const GreenNode *TreeBuilder::finish() {
  assert(node_stack_.empty() && "finish() with open nodes remaining");
  assert(elements_.size() == 1 && "finish() expects exactly one root element");
  assert(elements_[0].is_node() && "root element must be a node");
  return elements_[0].as_node();
}

} // namespace cst
} // namespace sammine_lang
