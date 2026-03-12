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

struct QualifiedName {
  struct Part {
    std::string name;
    std::string type_args; // "<i32>" including brackets, or ""
    std::string mangled() const { return name + type_args; }
  };

private:
  std::vector<Part> parts_; // [{math,""}, {Color,""}, {Red,""}]
  std::optional<std::string> module_alias_; // original alias before resolution
  bool unresolved_ = false;

  // local() is restricted to parsing/AST construction only.
  static QualifiedName local(std::string name) {
    QualifiedName qn;
    qn.parts_ = {{std::move(name), ""}};
    return qn;
  }

  friend class sammine_lang::Parser;
  friend class sammine_lang::AST::SimpleTypeExprAST;
  friend class sammine_lang::AST::TypeAliasDefAST;
  friend class sammine_lang::AST::CallExprAST;
  friend class sammine_lang::AST::StructLiteralExprAST;
  friend struct sammine_lang::AST::CasePattern;

public:
  QualifiedName() : parts_{{"", ""}} {}

  static QualifiedName qualified(std::string module, std::string name) {
    QualifiedName qn;
    qn.parts_ = {{std::move(module), ""}, {std::move(name), ""}};
    return qn;
  }

  static QualifiedName unresolved_qualified(std::string alias,
                                            std::string name) {
    QualifiedName qn;
    qn.parts_ = {{std::move(alias), ""}, {std::move(name), ""}};
    qn.unresolved_ = true;
    return qn;
  }

  // Convenience: create from plain strings (no type args)
  static QualifiedName from_parts(std::vector<std::string> names,
                                  bool unresolved = false,
                                  std::optional<std::string> module_alias = std::nullopt) {
    QualifiedName qn;
    qn.parts_.clear();
    qn.parts_.reserve(names.size());
    for (auto &n : names)
      qn.parts_.push_back({std::move(n), ""});
    qn.unresolved_ = unresolved;
    qn.module_alias_ = std::move(module_alias);
    return qn;
  }

  // Create from structured Parts (with type args)
  static QualifiedName from_parts(std::vector<Part> parts,
                                  bool unresolved = false,
                                  std::optional<std::string> module_alias = std::nullopt) {
    QualifiedName qn;
    qn.parts_ = std::move(parts);
    qn.unresolved_ = unresolved;
    qn.module_alias_ = std::move(module_alias);
    return qn;
  }

  // Return a copy with type_args set on the last part
  QualifiedName with_type_args(std::string ta) const {
    auto copy = *this;
    copy.parts_.back().type_args = std::move(ta);
    return copy;
  }

  // Return a copy with all type_args cleared
  QualifiedName strip_type_args() const {
    auto copy = *this;
    for (auto &p : copy.parts_)
      p.type_args.clear();
    return copy;
  }

  // Return a copy with an additional part appended
  QualifiedName with_appended(Part part) const {
    auto copy = *this;
    copy.parts_.push_back(std::move(part));
    return copy;
  }

  // Accessors
  const std::string &get_name() const { return parts_.back().name; }
  std::string get_module() const {
    return parts_.size() > 1 ? parts_.front().name : "";
  }
  std::string get_qualifier() const {
    if (parts_.size() <= 1)
      return "";
    std::string result = parts_[0].mangled();
    for (size_t i = 1; i + 1 < parts_.size(); i++)
      result += "::" + parts_[i].mangled();
    return result;
  }
  const std::vector<Part> &parts() const { return parts_; }
  size_t depth() const { return parts_.size(); }

  // Query methods
  bool is_qualified() const { return parts_.size() > 1; }
  bool is_unresolved() const { return unresolved_; }

  // String representation — includes type_args in each part
  std::string mangled() const {
    std::string result = parts_[0].mangled();
    for (size_t i = 1; i < parts_.size(); i++)
      result += "::" + parts_[i].mangled();
    return result;
  }

  // Return a copy with the original alias swapped back in
  QualifiedName with_alias() const {
    if (module_alias_) {
      auto copy = *this;
      copy.parts_[0].name = *module_alias_;
      return copy;
    }
    return *this;
  }

  // Return a copy with module prepended (if not already qualified)
  QualifiedName with_module(const std::string &mod) const {
    if (is_qualified() || mod.empty())
      return *this;
    QualifiedName qn;
    qn.parts_ = {{mod, ""}, parts_.back()};
    return qn;
  }
};

} // namespace sammine_util
