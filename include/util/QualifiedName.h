#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct Type;

// Forward declarations for QualifiedName friends
namespace sammine_lang {
class Parser;
namespace AST {
class SimpleTypeExprAST;
class TypeAliasDefAST;
class CallExprAST;
class VariableExprAST;
class StructLiteralExprAST;
struct CasePattern;
} // namespace AST
} // namespace sammine_lang

namespace sammine_util {

/// What a definition is called: its path, each part with the types it is
/// applied to. `vec::push<i32>` is [vec, push applied to (i32)], and
/// `Drop<Vec<i32>>::Drop` is [Drop applied to (Vec<i32>), Drop]. Two names
/// are the same name when their parts and those types are: a name is a key
/// as it is (QualifiedNameHash), and `mangled()` only shows it.
///
/// Its functions are defined at the end of typecheck/Types.h, where Type is
/// whole.
struct QualifiedName {
  struct Part {
    std::string name;
    std::vector<Type> type_args;
    /// `name`, then `<A, B>` if it is applied to types.
    std::string mangled() const;
    bool operator==(const Part &other) const;
  };

private:
  std::vector<Part> parts_; // [{math}, {Color}, {Red}]

  // local() is restricted to parsing/AST construction only.
  static QualifiedName local(std::string name);

  friend class sammine_lang::Parser;
  friend class sammine_lang::AST::SimpleTypeExprAST;
  friend class sammine_lang::AST::TypeAliasDefAST;
  friend class sammine_lang::AST::CallExprAST;
  friend class sammine_lang::AST::VariableExprAST;
  friend class sammine_lang::AST::StructLiteralExprAST;
  friend struct sammine_lang::AST::CasePattern;

public:
  QualifiedName();

  static QualifiedName qualified(std::string module, std::string name);

  // Convenience: create from plain strings (no type args)
  static QualifiedName from_parts(std::vector<std::string> names);

  // Create from structured Parts (with type args)
  static QualifiedName from_parts(std::vector<Part> parts);

  // Return a copy whose last part is applied to `types`
  QualifiedName with_type_args(std::vector<Type> types) const;

  // Return a copy with an additional part appended
  QualifiedName with_appended(Part part) const;

  // Accessors
  const std::string &get_name() const { return parts_.back().name; }
  std::string get_module() const;
  std::string get_qualifier() const;
  /// What the name is a member of: the name without its last part.
  QualifiedName qualifier() const;
  const std::vector<Part> &parts() const { return parts_; }
  size_t depth() const { return parts_.size(); }

  // Query methods
  bool is_qualified() const { return parts_.size() > 1; }

  bool operator==(const QualifiedName &other) const;

  /// The name as source writes it, `a::b<T>::c`: for messages, and for the
  /// symbol a definition is linked as.
  std::string mangled() const;
};

/// A name as text, for a message.
std::string name_text(const QualifiedName &name);

struct QualifiedNameHash {
  size_t operator()(const QualifiedName &name) const;
};

} // namespace sammine_util

// A name's parts hold types, so using one needs Type whole.
#include "typecheck/Types.h"
