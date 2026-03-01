#pragma once
#include <string>
#include <utility>
#include <vector>

namespace sammine_util {

struct QualifiedName {
private:
  std::vector<std::string> parts_; // ["math", "Color", "Red"]
  bool unresolved_ = false;

public:
  QualifiedName() : parts_{""} {}

  // Factory methods
  static QualifiedName local(std::string name) {
    QualifiedName qn;
    qn.parts_ = {std::move(name)};
    return qn;
  }

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
                                  bool unresolved = false) {
    QualifiedName qn;
    qn.parts_ = std::move(parts);
    qn.unresolved_ = unresolved;
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

  // Return a copy with module prepended (if not already qualified)
  QualifiedName with_module(const std::string &mod) const {
    if (is_qualified() || mod.empty())
      return *this;
    return qualified(mod, parts_.back());
  }
};

} // namespace sammine_util
