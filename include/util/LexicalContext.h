#pragma once
#include "util/Utilities.h"
#include <functional>
#include <memory>
#include <ostream>
#include <set>

//! \file LexicalContext.h
//! \brief A simple scoping class, doesn't differentiate between different
//! names, like variable name, func name and all that

//! \brief NameQueryResult enum
enum NameQueryResult {
  nameFound,
  nameNotFound,
};

//! \brief LexicalContext class
template <class T, class Scope> class LexicalContext {
  std::set<std::string> symbols;
  std::unordered_map<std::string, T> symbols_to_t;

public:
  LexicalContext *parent_scope;
  std::optional<Scope> s; 

  explicit LexicalContext() : symbols(), symbols_to_t(), parent_scope() {}
  LexicalContext(LexicalContext *parent_scope)
      : symbols(), symbols_to_t(), parent_scope(parent_scope) {}

  void setScope(Scope s) {
    this->s = s;
  }

  void registerNameT(const std::string &name, T l) {
    symbols.insert(name);
    symbols_to_t[name] = l;
  }
  NameQueryResult queryName(const std::string &name) const {
    return symbols.contains(name) ? nameFound : nameNotFound;
  }
  T get_from_name(const std::string &name) { return symbols_to_t.at(name); }

  T recursive_get_from_name(const std::string &name) {
    if (symbols.find(name) != symbols.end())
      return symbols_to_t[name];
    else if (parent_scope == nullptr) {
      sammine_util::abort(fmt::format("name {} not found", name));
    } else
      return parent_scope->recursive_get_from_name(name);
  }

  NameQueryResult recursiveQueryName(const std::string &name) const {
    if (symbols.find(name) != symbols.end())
      return nameFound;
    else if (parent_scope == nullptr) {
      return nameNotFound;
    } else
      return parent_scope->recursiveQueryName(name);
  }
}; // namespace sammine_lang::AST
