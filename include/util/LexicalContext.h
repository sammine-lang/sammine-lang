#pragma once
#include "util/Utilities.h"
#include <functional>
#include <memory>
#include <optional>
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

/// A name as text, for a message.
inline const std::string &name_text(const std::string &name) { return name; }

//! \brief A scope: what each name bound in it stands for, and the scope
//! around it. Names are `K`: a string, or a name that is a key as it is.
template <class T, class K = std::string, class Hash = std::hash<K>>
class LexicalContext {
  std::unordered_map<K, T, Hash> symbols_to_t;

public:
  LexicalContext *parent_scope;

  explicit LexicalContext() : symbols_to_t(), parent_scope() {}
  LexicalContext(LexicalContext *parent_scope_)
      : symbols_to_t(), parent_scope(parent_scope_) {}

  void registerNameT(const K &name, T l) { symbols_to_t[name] = l; }
  NameQueryResult queryName(const K &name) const {
    return symbols_to_t.contains(name) ? nameFound : nameNotFound;
  }
  T get_from_name(const K &name) const { return symbols_to_t.at(name); }

  T recursive_get_from_name(const K &name) const {
    if (auto found = recursive_try_get(name))
      return *found;
    sammine_util::abort(fmt::format("name {} not found", name_text(name)));
  }

  NameQueryResult recursiveQueryName(const K &name) const {
    return recursive_try_get(name) ? nameFound : nameNotFound;
  }

  std::optional<T> try_get(const K &name) const {
    auto it = symbols_to_t.find(name);
    return it != symbols_to_t.end() ? std::optional<T>(it->second)
                                    : std::nullopt;
  }

  std::optional<T> recursive_try_get(const K &name) const {
    auto it = symbols_to_t.find(name);
    if (it != symbols_to_t.end())
      return it->second;
    return parent_scope ? parent_scope->recursive_try_get(name) : std::nullopt;
  }
};
