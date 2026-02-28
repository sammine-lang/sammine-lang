#include "cst/GreenNode.h"
#include <cstdlib>
#include <new>

namespace sammine_lang {
namespace cst {

// --- GreenElement ---

uint32_t GreenElement::text_len() const {
  if (is_token())
    return as_token()->text_len();
  if (is_node())
    return as_node()->text_len();
  return 0;
}

SyntaxKind GreenElement::kind() const {
  if (is_token())
    return as_token()->kind();
  if (is_node())
    return as_node()->kind();
  return SyntaxKind::TokINVALID;
}

// --- GreenToken ---

const GreenToken *
GreenToken::create(std::pmr::monotonic_buffer_resource &arena, SyntaxKind kind,
                   std::string_view text) {
  size_t size = sizeof(GreenToken) + text.size();
  // Ensure 8-byte alignment for the tag-pointer scheme
  void *mem = arena.allocate(size, alignof(GreenToken));
  return new (mem) GreenToken(kind, text);
}

// --- GreenNode ---

const GreenNode *
GreenNode::create(std::pmr::monotonic_buffer_resource &arena, SyntaxKind kind,
                  const GreenElement *children, uint32_t num_children) {
  size_t size = sizeof(GreenNode) + sizeof(GreenElement) * num_children;
  void *mem = arena.allocate(size, alignof(GreenNode));
  return new (mem) GreenNode(kind, children, num_children);
}

// --- GreenInterner ---

const GreenToken *GreenInterner::token(SyntaxKind kind,
                                       std::string_view text) {
  TokenKey key{kind, std::string(text)};
  auto it = token_cache_.find(key);
  if (it != token_cache_.end())
    return it->second;

  auto *tok = GreenToken::create(arena_, kind, text);
  token_cache_.emplace(std::move(key), tok);
  return tok;
}

const GreenNode *
GreenInterner::node(SyntaxKind kind,
                    const std::vector<GreenElement> &children) {
  return GreenNode::create(arena_, kind, children.data(),
                           static_cast<uint32_t>(children.size()));
}

} // namespace cst
} // namespace sammine_lang
