#pragma once
#include <string>
#include <utility>

namespace sammine_util {

struct QualifiedName {
  std::string module; // "" for local, "math" for resolved, "x" for unresolved
  std::string name;   // "add"
  bool unresolved = false; // true when alias wasn't in alias_to_module

  static QualifiedName local(std::string name) {
    return QualifiedName{"", std::move(name), false};
  }

  static QualifiedName qualified(std::string module, std::string name) {
    return QualifiedName{std::move(module), std::move(name), false};
  }

  static QualifiedName unresolved_qualified(std::string alias,
                                            std::string name) {
    return QualifiedName{std::move(alias), std::move(name), true};
  }

  bool is_qualified() const { return !module.empty(); }
  bool is_unresolved() const { return unresolved; }

  std::string mangled() const {
    if (module.empty())
      return name;
    return module + "::" + name;
  }

  std::string display() const {
    if (module.empty())
      return name;
    return module + "::" + name;
  }

  // Parse a mangled "module__name" string back into a QualifiedName
  static QualifiedName from_mangled(const std::string &mangled) {
    auto pos = mangled.find("__");
    if (pos == std::string::npos)
      return local(mangled);
    return qualified(mangled.substr(0, pos), mangled.substr(pos + 2));
  }

  // Return a copy with module set (if not already qualified)
  QualifiedName with_module(const std::string &mod) const {
    if (!module.empty())
      return *this;
    return qualified(mod, name);
  }
};

} // namespace sammine_util
