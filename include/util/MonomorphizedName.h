#pragma once
#include "util/QualifiedName.h"
#include <string>

namespace sammine_util {

/// Encapsulates the naming of monomorphized generics and typeclass instances.
/// Separates monomorphization naming from QualifiedName (which handles scope
/// resolution). After scope generation, all compiler-generated names should go
/// through this class — never QualifiedName::local().
///
/// Mangling formats:
///   generic:   "math::identity<i32>"     (base.mangled() + type_args)
///   typeclass: "Add<i32>::add"           (base.mangled() + type_args + :: + method)
struct MonomorphizedName {
  QualifiedName base;      // original name (preserves module)
  std::string type_args;   // "<i32>" or "<Option<i32>>" (includes brackets)
  std::string method_name; // "add" for typeclasses, "" for generics

  /// Generic function/enum: identity<i32>, Option<i32>
  static MonomorphizedName generic(QualifiedName base, std::string type_args) {
    return {std::move(base), std::move(type_args), ""};
  }

  /// Typeclass instance method: Add<i32>::add
  static MonomorphizedName typeclass(std::string class_name,
                                     std::string concrete_type_str,
                                     std::string method_name) {
    return {QualifiedName::local(std::move(class_name)),
            "<" + std::move(concrete_type_str) + ">",
            std::move(method_name)};
  }

  /// Full mangled string for lookups and codegen symbol names.
  ///   generic:   "math::identity<i32>"
  ///   typeclass: "Add<i32>::add"
  std::string mangled() const {
    if (method_name.empty())
      return base.mangled() + type_args;
    return base.mangled() + type_args + "::" + method_name;
  }

  /// Produce a QualifiedName suitable for prototype functionName / codegen.
  ///   generic:   qualified("math", "identity<i32>") — module preserved
  ///   typeclass: local("Add<i32>::add") — single part for mangleName() compat
  QualifiedName to_qualified_name() const {
    if (method_name.empty()) {
      // Generic: replace the last part of base with name + type_args
      auto parts = base.parts();
      parts.back() = base.get_name() + type_args;
      return QualifiedName::from_parts(std::move(parts));
    }
    // Typeclass: single-part local name so mangleName() can prepend module
    return QualifiedName::local(base.mangled() + type_args + "::" +
                                method_name);
  }
};

} // namespace sammine_util
