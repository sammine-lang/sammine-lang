#pragma once

#include "ast/AstDecl.h"
#include "typecheck/Types.h"
#include "util/FormatString.h"
#include "util/QualifiedName.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace sammine_lang {
namespace AST {

/// What a name used as a value means: the callee of a call, or a variable
/// expression. The type checker decides it, once; every later stage reads it
/// and none looks the name up again.
struct Reference {
  /// A parameter or a `let` of the function the name is used in.
  struct Local {};
  /// A function, by its definition's name: a function or a C binding, a
  /// generic function's copy for its types (id<i32>), an instance's method
  /// (Add<Point>::add).
  struct Function {
    sammine_util::QualifiedName name;
  };
  /// A variant of the enum the expression has the type of, by its index.
  struct Variant {
    size_t index;
  };
  /// A method of an instance the compiler provides, such as Add<i32>, which
  /// has no function to call. Only a call reaches one.
  struct Builtin {
    MonomorphizedKey instance;
  };
  /// Nothing, until the checker resolves the name; it stays so if it can't.
  std::variant<std::monostate, Local, Function, Variant, Builtin> target;

  template <typename T> const T *as() const { return std::get_if<T>(&target); }
  template <typename T> bool is() const {
    return std::holds_alternative<T>(target);
  }
};

struct CallProps {
  /// What is called.
  Reference callee;
  std::optional<Type> callee_func_type;
  bool is_partial = false;
};

struct VariableProps {
  /// What the variable expression is the value of.
  Reference value;
};

struct BinaryProps {
  /// For an operator through a typeclass (`+` through Add): the instance's
  /// method, a function or built in. Nothing for one that goes through none.
  Reference method;
};

struct IndexProps {
  /// For x[i] through an Indexer instance: the method it calls with &x and
  /// i. Nothing for an array's or a pointer's own indexing.
  Reference method;
  bool through_instance() const { return method.is<Reference::Function>(); }
};

struct CastProps {
  /// The method of the Convert<From, To> instance the cast goes through.
  Reference method;
};

struct FormatProps {
  /// The format, as the type checker read it. Each argument it prints has
  /// an instance of Format for its type, which the checker made sure of.
  std::optional<sammine_util::FormatString> format;
};

struct TypeAliasProps {
  Type resolved_type = Type::NonExistent();
};

struct ForProps {
  /// The `Range` struct definition this loop iterates with.
  std::optional<Type> range_type;
};

class ASTProperties {
  std::unordered_map<NodeId, CallProps> call_props_;
  std::unordered_map<NodeId, VariableProps> variable_props_;
  std::unordered_map<NodeId, BinaryProps> binary_props_;
  std::unordered_map<NodeId, CastProps> cast_props_;
  std::unordered_map<NodeId, FormatProps> format_props_;
  std::unordered_map<NodeId, IndexProps> index_props_;
  std::unordered_map<NodeId, TypeAliasProps> type_alias_props_;
  std::unordered_map<NodeId, ForProps> for_props_;
  std::unordered_map<NodeId, Type> node_types_;
  std::unordered_map<sammine_util::QualifiedName, std::string,
                     sammine_util::QualifiedNameHash>
      symbols_;
  std::optional<sammine_util::QualifiedName> entry_point_;

public:
  // Mutable accessors — auto-insert a default-constructed entry
  CallProps &call(NodeId id) { return call_props_[id]; }
  VariableProps &variable(NodeId id) { return variable_props_[id]; }
  BinaryProps &binary(NodeId id) { return binary_props_[id]; }
  CastProps &cast(NodeId id) { return cast_props_[id]; }
  FormatProps &format(NodeId id) { return format_props_[id]; }
  IndexProps &index(NodeId id) { return index_props_[id]; }
  TypeAliasProps &type_alias(NodeId id) { return type_alias_props_[id]; }
  ForProps &for_loop(NodeId id) { return for_props_[id]; }

  // Const accessors — return pointer (nullptr if missing)
  const CallProps *call(NodeId id) const {
    auto it = call_props_.find(id);
    return it != call_props_.end() ? &it->second : nullptr;
  }
  const VariableProps *variable(NodeId id) const {
    auto it = variable_props_.find(id);
    return it != variable_props_.end() ? &it->second : nullptr;
  }
  const BinaryProps *binary(NodeId id) const {
    auto it = binary_props_.find(id);
    return it != binary_props_.end() ? &it->second : nullptr;
  }
  const CastProps *cast(NodeId id) const {
    auto it = cast_props_.find(id);
    return it != cast_props_.end() ? &it->second : nullptr;
  }
  const FormatProps *format(NodeId id) const {
    auto it = format_props_.find(id);
    return it != format_props_.end() ? &it->second : nullptr;
  }
  const IndexProps *index(NodeId id) const {
    auto it = index_props_.find(id);
    return it != index_props_.end() ? &it->second : nullptr;
  }
  const TypeAliasProps *type_alias(NodeId id) const {
    auto it = type_alias_props_.find(id);
    return it != type_alias_props_.end() ? &it->second : nullptr;
  }
  const ForProps *for_loop(NodeId id) const {
    auto it = for_props_.find(id);
    return it != for_props_.end() ? &it->second : nullptr;
  }

  //! The function a program starts at, linked as `symbol`, which C's
  //! runtime calls: NameResolver decides which it is.
  void set_entry_point(const sammine_util::QualifiedName &name,
                       std::string symbol) {
    entry_point_ = name;
    set_symbol(name, std::move(symbol));
  }
  bool is_entry_point(const sammine_util::QualifiedName &name) const {
    return entry_point_ == name;
  }

  //! The symbol the definition named `name` is linked as, where NameResolver
  //! decides it isn't the name's own: a C binding's is its C name, in every
  //! module that binds it (std::printf and io::printf are both printf).
  void set_symbol(const sammine_util::QualifiedName &name, std::string symbol) {
    symbols_[name] = std::move(symbol);
  }
  //! The symbol for `name`, a definition's resolved name: its own, unless
  //! NameResolver set another.
  std::string symbol(const sammine_util::QualifiedName &name) const {
    auto it = symbols_.find(name);
    return it != symbols_.end() ? it->second : name.symbol();
  }

  // Type accessors (for future AstBase::type migration)
  void set_type(NodeId id, const Type &t) { node_types_[id] = t; }
  Type get_type(NodeId id) const {
    auto it = node_types_.find(id);
    return it != node_types_.end() ? it->second : Type::NonExistent();
  }
};

} // namespace AST
} // namespace sammine_lang
