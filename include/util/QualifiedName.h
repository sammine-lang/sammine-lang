#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Forward declarations for QualifiedName friends
namespace sammine_lang {
class Parser;
namespace AST {
class SimpleTypeExprAST;
class TypeAliasDefAST;
class CallExprAST;
class StructLiteralExprAST;
struct CasePattern;
} // namespace AST
} // namespace sammine_lang

namespace sammine_util {

struct MonomorphizedName;

struct QualifiedName {
private:
  std::vector<std::string> parts_; // ["math", "Color", "Red"]
  std::optional<std::string> module_alias_; // original alias before resolution
  bool unresolved_ = false;

  // local() is restricted to parsing/AST construction only.
  // After scope generation, use MonomorphizedName instead.
  static QualifiedName local(std::string name) {
    QualifiedName qn;
    qn.parts_ = {std::move(name)};
    return qn;
  }

  friend class sammine_lang::Parser;
  friend class sammine_lang::AST::SimpleTypeExprAST;
  friend class sammine_lang::AST::TypeAliasDefAST;
  friend class sammine_lang::AST::CallExprAST;
  friend class sammine_lang::AST::StructLiteralExprAST;
  friend struct sammine_lang::AST::CasePattern;
  friend struct sammine_util::MonomorphizedName;

public:
  QualifiedName() : parts_{""} {}

  static QualifiedName qualified(std::string module, std::string name) {
    QualifiedName qn;
    qn.parts_ = {std::move(module), std::move(name)};
    return qn;
  }

  static QualifiedName unresolved_qualified(std::string alias,
                                            std::string name) {
    QualifiedName qn;
    qn.parts_ = {std::move(alias), std::move(name)};
    qn.unresolved_ = true;
    return qn;
  }

  static QualifiedName from_parts(std::vector<std::string> parts,
                                  bool unresolved = false,
                                  std::optional<std::string> module_alias = std::nullopt) {
    QualifiedName qn;
    qn.parts_ = std::move(parts);
    qn.unresolved_ = unresolved;
    qn.module_alias_ = std::move(module_alias);
    return qn;
  }

  // Accessors
  const std::string &get_name() const { return parts_.back(); }
  std::string get_module() const {
    return parts_.size() > 1 ? parts_.front() : "";
  }
  std::string get_qualifier() const {
    if (parts_.size() <= 1)
      return "";
    std::string result = parts_[0];
    for (size_t i = 1; i + 1 < parts_.size(); i++)
      result += "::" + parts_[i];
    return result;
  }
  const std::vector<std::string> &parts() const { return parts_; }
  size_t depth() const { return parts_.size(); }

  // Query methods
  bool is_qualified() const { return parts_.size() > 1; }
  bool is_unresolved() const { return unresolved_; }

  // String representation — single method
  std::string mangled() const {
    std::string result = parts_[0];
    for (size_t i = 1; i < parts_.size(); i++)
      result += "::" + parts_[i];
    return result;
  }

  // Return a copy with the original alias swapped back in
  QualifiedName with_alias() const {
    if (module_alias_) {
      auto copy = *this;
      copy.parts_[0] = *module_alias_;
      return copy;
    }
    return *this;
  }

  // Return a copy with module prepended (if not already qualified)
  QualifiedName with_module(const std::string &mod) const {
    if (is_qualified() || mod.empty())
      return *this;
    return qualified(mod, parts_.back());
  }
};

} // namespace sammine_util
