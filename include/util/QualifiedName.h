#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
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

/// The two ways a name is spelled. Written is as source writes it, for
/// messages: a program's own and the prelude's without their module. Linked
/// is the name whole, the types it is applied to as well, so no two names
/// are spelled alike: what a symbol is.
enum class NameForm { Written, Linked };

/// What a definition is called: its path, each part with the types it is
/// applied to. `vec::push<i32>` is [vec, push applied to (i32)], and
/// `Drop<Vec<i32>>::Drop` is [Drop applied to (Vec<i32>), Drop]. Two names
/// are the same name when their parts and those types are: a name is a key
/// as it is (QualifiedNameHash), and `mangled()` only shows it.
///
/// Every definition's name starts with its module, with no exception. A
/// program, which is no module, has the module with no name: its `helper` is
/// [, helper], linked as `::helper`. The prelude's `Option` is
/// [definitions, Option]. Both are written, and shown, without the module:
/// `helper`, `Option`. So a name of one part is never a definition's: it is
/// a local, a type parameter or a built-in type; and no function's symbol
/// is a C function's name.
///
/// Its functions are defined at the end of typecheck/Types.h, where Type is
/// whole.
struct QualifiedName {
  struct Part {
    std::string name;
    std::vector<Type> type_args;
    /// `name`, then `<A, B>` if it is applied to types.
    std::string mangled() const;
    std::string spelled(NameForm form) const;
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

  /// The prelude's module: every module imports it without writing so, and
  /// writes its names without it.
  static constexpr std::string_view kPreludeModule = "definitions";

  static QualifiedName qualified(std::string module, std::string name);
  /// The name of the prelude's `name`: definitions::Option.
  static QualifiedName prelude(std::string_view name);

  // Convenience: create from plain strings (no type args)
  static QualifiedName from_parts(std::vector<std::string> names);

  // Create from structured Parts (with type args)
  static QualifiedName from_parts(std::vector<Part> parts);

  // Return a copy whose last part is applied to `types`
  QualifiedName with_type_args(std::vector<Type> types) const;

  // Return a copy with an additional part appended
  QualifiedName with_appended(Part part) const;
  /// The name `name` has beside this one, in the same module: m::Area's
  /// sibling `area` is m::area.
  QualifiedName sibling(std::string name) const;

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
  /// Whether this is a name in a program's own code, the module with no
  /// name.
  bool is_of_program() const {
    return is_qualified() && parts_.front().name.empty();
  }
  /// Whether this is a name of the prelude.
  bool is_of_prelude() const {
    return is_qualified() && parts_.front().name == kPreludeModule;
  }

  bool operator==(const QualifiedName &other) const;

  /// The name as source writes it, `a::b<T>::c`, for messages. A program's
  /// own and the prelude's are written without a module: `helper`, `Option`.
  std::string mangled() const;
  /// The symbol a definition of this name is linked as: every part,
  /// `a::b<T>::c`, a program's own as `::helper`, and the types in it by
  /// their whole names too, `definitions::Hash<::Mine>::Hash`.
  std::string symbol() const;
  /// The name in `form`, `::` between its parts.
  std::string spelled(NameForm form) const;
};

/// A name as text, for a message.
std::string name_text(const QualifiedName &name);

struct QualifiedNameHash {
  size_t operator()(const QualifiedName &name) const;
};

} // namespace sammine_util

// A name's parts hold types, so using one needs Type whole.
#include "typecheck/Types.h"
