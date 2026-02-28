#include "cst/SyntaxNode.h"
#include <cassert>

namespace sammine_lang {
namespace cst {

std::vector<SyntaxChild> SyntaxNode::children() const {
  std::vector<SyntaxChild> result;
  result.reserve(green_->num_children());
  uint32_t child_offset = offset_;
  for (auto it = green_->children_begin(); it != green_->children_end();
       ++it) {
    result.push_back({*it, child_offset});
    child_offset += it->text_len();
  }
  return result;
}

std::optional<SyntaxNode> SyntaxNode::child_node(SyntaxKind kind) const {
  uint32_t child_offset = offset_;
  for (auto it = green_->children_begin(); it != green_->children_end();
       ++it) {
    if (it->is_node() && it->kind() == kind) {
      return SyntaxNode(it->as_node(), this, child_offset);
    }
    child_offset += it->text_len();
  }
  return std::nullopt;
}

std::optional<std::string_view>
SyntaxNode::child_token_text(SyntaxKind kind) const {
  for (auto it = green_->children_begin(); it != green_->children_end();
       ++it) {
    if (it->is_token() && it->kind() == kind) {
      return it->as_token()->text();
    }
  }
  return std::nullopt;
}

std::string SyntaxNode::text() const {
  std::string result;
  result.reserve(text_len());

  // Recursively collect all leaf text
  struct Collector {
    std::string &out;
    void visit(const GreenElement &elem) {
      if (elem.is_token()) {
        auto sv = elem.as_token()->text();
        out.append(sv.data(), sv.size());
      } else if (elem.is_node()) {
        auto *node = elem.as_node();
        for (auto it = node->children_begin(); it != node->children_end();
             ++it) {
          visit(*it);
        }
      }
    }
  };

  Collector collector{result};
  for (auto it = green_->children_begin(); it != green_->children_end();
       ++it) {
    collector.visit(*it);
  }
  return result;
}

SyntaxNode SyntaxNode::node_at_offset(uint32_t byte_offset) const {
  assert(byte_offset >= offset_ && byte_offset < end_offset());

  uint32_t child_offset = offset_;
  for (auto it = green_->children_begin(); it != green_->children_end();
       ++it) {
    uint32_t child_end = child_offset + it->text_len();
    if (byte_offset >= child_offset && byte_offset < child_end) {
      if (it->is_node()) {
        SyntaxNode child(it->as_node(), this, child_offset);
        return child.node_at_offset(byte_offset);
      }
      // It's a token — return the current node (tokens don't have children)
      return *this;
    }
    child_offset = child_end;
  }
  // Shouldn't reach here if byte_offset is in range, but return self
  return *this;
}

void SyntaxNode::dump(std::string &out, int indent) const {
  for (int i = 0; i < indent; i++)
    out += "  ";
  out += std::string(syntax_kind_name(kind()));
  out += "@";
  out += std::to_string(offset_);
  out += "..";
  out += std::to_string(end_offset());
  out += "\n";

  uint32_t child_offset = offset_;
  for (auto it = green_->children_begin(); it != green_->children_end();
       ++it) {
    if (it->is_token()) {
      for (int i = 0; i < indent + 1; i++)
        out += "  ";
      out += std::string(syntax_kind_name(it->kind()));
      out += " \"";
      // Escape special characters in token text for display
      auto text = it->as_token()->text();
      for (char c : text) {
        if (c == '\n')
          out += "\\n";
        else if (c == '\r')
          out += "\\r";
        else if (c == '\t')
          out += "\\t";
        else if (c == '"')
          out += "\\\"";
        else
          out += c;
      }
      out += "\"";
      out += "@";
      out += std::to_string(child_offset);
      out += "..";
      out += std::to_string(child_offset + it->text_len());
      out += "\n";
    } else if (it->is_node()) {
      SyntaxNode child(it->as_node(), this, child_offset);
      child.dump(out, indent + 1);
    }
    child_offset += it->text_len();
  }
}

std::string SyntaxNode::dump() const {
  std::string result;
  dump(result);
  return result;
}

} // namespace cst
} // namespace sammine_lang
