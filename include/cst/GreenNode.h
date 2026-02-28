#pragma once

#include "cst/SyntaxKind.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sammine_lang {
namespace cst {

class GreenToken;
class GreenNode;

/// A tagged pointer that is either a GreenToken* or a GreenNode*.
/// Uses the low bit to discriminate (0 = token, 1 = node).
class GreenElement {
  uintptr_t ptr_ = 0;

public:
  GreenElement() = default;
  explicit GreenElement(const GreenToken *t)
      : ptr_(reinterpret_cast<uintptr_t>(t)) {
    assert((ptr_ & 1) == 0 && "GreenToken must be 2-byte aligned");
  }
  explicit GreenElement(const GreenNode *n)
      : ptr_(reinterpret_cast<uintptr_t>(n) | 1) {
    assert((reinterpret_cast<uintptr_t>(n) & 1) == 0 &&
           "GreenNode must be 2-byte aligned");
  }

  bool is_token() const { return (ptr_ & 1) == 0 && ptr_ != 0; }
  bool is_node() const { return (ptr_ & 1) == 1; }
  bool is_null() const { return ptr_ == 0; }

  const GreenToken *as_token() const {
    assert(is_token());
    return reinterpret_cast<const GreenToken *>(ptr_);
  }
  const GreenNode *as_node() const {
    assert(is_node());
    return reinterpret_cast<const GreenNode *>(ptr_ & ~uintptr_t(1));
  }

  /// Returns the text length of this element (token or node)
  uint32_t text_len() const;

  /// Returns the SyntaxKind of this element
  SyntaxKind kind() const;

  bool operator==(const GreenElement &other) const {
    return ptr_ == other.ptr_;
  }
};

/// A leaf node in the green tree: a token with its literal text.
/// Allocated in the arena with flexible array member for the text.
class GreenToken {
  SyntaxKind kind_;
  uint32_t text_len_;
  // Flexible array member: text bytes follow immediately after
  char text_[];

  GreenToken(SyntaxKind kind, std::string_view text)
      : kind_(kind), text_len_(static_cast<uint32_t>(text.size())) {
    std::memcpy(text_, text.data(), text.size());
  }

public:
  SyntaxKind kind() const { return kind_; }
  uint32_t text_len() const { return text_len_; }
  std::string_view text() const { return {text_, text_len_}; }

  /// Allocate a GreenToken in the given arena
  static const GreenToken *create(std::pmr::monotonic_buffer_resource &arena,
                                  SyntaxKind kind, std::string_view text);
};

/// An interior node in the green tree: has a kind, cached text length,
/// and a flexible array of children.
class GreenNode {
  SyntaxKind kind_;
  uint32_t text_len_;
  uint32_t num_children_;
  // Flexible array member: GreenElement children follow immediately
  GreenElement children_[];

  GreenNode(SyntaxKind kind, const GreenElement *children,
            uint32_t num_children)
      : kind_(kind), num_children_(num_children) {
    text_len_ = 0;
    for (uint32_t i = 0; i < num_children; i++) {
      children_[i] = children[i];
      text_len_ += children[i].text_len();
    }
  }

public:
  SyntaxKind kind() const { return kind_; }
  uint32_t text_len() const { return text_len_; }
  uint32_t num_children() const { return num_children_; }

  const GreenElement *children_begin() const { return children_; }
  const GreenElement *children_end() const {
    return children_ + num_children_;
  }

  const GreenElement &child(uint32_t index) const {
    assert(index < num_children_);
    return children_[index];
  }

  /// Allocate a GreenNode in the given arena
  static const GreenNode *create(std::pmr::monotonic_buffer_resource &arena,
                                 SyntaxKind kind, const GreenElement *children,
                                 uint32_t num_children);
};

/// Arena allocator + structural dedup cache for green tree nodes.
/// Tokens with the same (kind, text) share the same GreenToken*.
/// Nodes with the same (kind, children) share the same GreenNode*.
class GreenInterner {
  std::pmr::monotonic_buffer_resource arena_;

  // Token dedup: key = (kind, text)
  struct TokenKey {
    SyntaxKind kind;
    std::string text;
    bool operator==(const TokenKey &other) const {
      return kind == other.kind && text == other.text;
    }
  };
  struct TokenKeyHash {
    size_t operator()(const TokenKey &k) const {
      size_t h = std::hash<uint16_t>{}(static_cast<uint16_t>(k.kind));
      h ^= std::hash<std::string>{}(k.text) + 0x9e3779b9 + (h << 6) +
           (h >> 2);
      return h;
    }
  };
  std::unordered_map<TokenKey, const GreenToken *, TokenKeyHash> token_cache_;

public:
  GreenInterner() : arena_(4096) {}

  /// Get or create a GreenToken with dedup
  const GreenToken *token(SyntaxKind kind, std::string_view text);

  /// Create a GreenNode (no dedup for nodes — structural sharing comes
  /// from shared children)
  const GreenNode *node(SyntaxKind kind, const std::vector<GreenElement> &children);
};

} // namespace cst
} // namespace sammine_lang
