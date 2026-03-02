#pragma once

#include "ast/AstDecl.h"
#include "typecheck/Types.h"
#include "util/MonomorphizedName.h"
#include <optional>
#include <string>
#include <unordered_map>

namespace sammine_lang {
namespace AST {

struct CallProps {
  std::optional<Type> callee_func_type;
  bool is_partial = false;
  std::optional<sammine_util::MonomorphizedName> resolved_name;
  std::unordered_map<std::string, Type> type_bindings;
  bool is_typeclass_call = false;
  bool is_enum_constructor = false;
  size_t enum_variant_index = 0;
};

struct VariableProps {
  bool is_enum_unit_variant = false;
  size_t enum_variant_index = 0;
};

struct BinaryProps {
  std::optional<sammine_util::MonomorphizedName> resolved_op_method;
};

struct TypeAliasProps {
  Type resolved_type = Type::NonExistent();
};

struct TypeClassInstanceProps {
  std::vector<Type> concrete_types;
};

class ASTProperties {
  std::unordered_map<NodeId, CallProps> call_props_;
  std::unordered_map<NodeId, VariableProps> variable_props_;
  std::unordered_map<NodeId, BinaryProps> binary_props_;
  std::unordered_map<NodeId, TypeAliasProps> type_alias_props_;
  std::unordered_map<NodeId, TypeClassInstanceProps> type_class_instance_props_;
  std::unordered_map<NodeId, Type> node_types_;

public:
  // Mutable accessors — auto-insert a default-constructed entry
  CallProps &call(NodeId id) { return call_props_[id]; }
  VariableProps &variable(NodeId id) { return variable_props_[id]; }
  BinaryProps &binary(NodeId id) { return binary_props_[id]; }
  TypeAliasProps &type_alias(NodeId id) { return type_alias_props_[id]; }
  TypeClassInstanceProps &type_class_instance(NodeId id) {
    return type_class_instance_props_[id];
  }

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
  const TypeAliasProps *type_alias(NodeId id) const {
    auto it = type_alias_props_.find(id);
    return it != type_alias_props_.end() ? &it->second : nullptr;
  }
  const TypeClassInstanceProps *type_class_instance(NodeId id) const {
    auto it = type_class_instance_props_.find(id);
    return it != type_class_instance_props_.end() ? &it->second : nullptr;
  }

  // Type accessors (for future AstBase::type migration)
  void set_type(NodeId id, const Type &t) { node_types_[id] = t; }
  Type get_type(NodeId id) const {
    auto it = node_types_.find(id);
    return it != node_types_.end() ? it->second : Type::NonExistent();
  }
  bool has_type(NodeId id) const { return node_types_.contains(id); }
};

} // namespace AST
} // namespace sammine_lang
