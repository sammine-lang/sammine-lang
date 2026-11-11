#pragma once
#include "ast/ASTContext.h"
#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "util/QualifiedName.h"
#include "util/Utilities.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

//! \file NameResolver.h
//! \brief Resolves a name written in a module's code to what it means.

namespace sammine_lang::AST {

//! The one place names are decided. It names every definition after its
//! module (shapes::Point), and resolves every name written in code to the
//! definition it means, rewriting it to that definition's name. Later stages
//! only read names. A name is resolved in the module whose code it is
//! (`where.module`), and that module's own definition comes first: code
//! imported from module M finds M's definitions, which are named M::X in the
//! importer, and an importer's same-named definition can't capture them.
//!
//! The names it looks up are the scope generator's: its scopes, whose root
//! holds the top-level definitions, and its table of variants to enums.
class NameResolver {
public:
  using Scopes = LexicalStack<sammine_util::Location>;
  using VariantEnums =
      std::map<std::string, std::vector<sammine_util::QualifiedName>>;

  NameResolver(Scopes &scopes, const VariantEnums &variant_to_enum,
               sammine_util::Reportee &reportee)
      : scopes(scopes), variant_to_enum(variant_to_enum), reportee(reportee) {}

  //! Name every definition of `program`, and take its modules' import
  //! aliases, before any name is registered or resolved.
  void use_program(ProgramAST &program);

  //! The prelude, stdlib/definitions.mn: every other module imports it
  //! without writing `import`, as Rust's prelude is, and its names are used
  //! without a prefix (Option, not definitions::Option), everywhere,
  //! including in its own library, so symbols built from them agree.
  static constexpr std::string_view kPrelude = "definitions";

  //! `name` as a definition of `module` is named: module::name. A name
  //! already qualified, of no module (an executable's) or of the prelude, is
  //! unchanged.
  static sammine_util::QualifiedName
  qualify(const sammine_util::QualifiedName &name, const std::string &module);

  //! A type, in a type expression, or the type a struct literal or a
  //! generic names.
  void resolve_type(TypeExprAST *type, const ASTContext &where);
  void resolve_type_name(sammine_util::QualifiedName &name,
                         sammine_util::Location location,
                         const ASTContext &where);
  //! A value: a call's callee or a variable. Reports a name not found, unless
  //! `has_type_args`: a typeclass method, which the type checker resolves.
  void resolve_value(sammine_util::QualifiedName &name, bool has_type_args,
                     bool is_call, sammine_util::Location location,
                     const ASTContext &where);
  //! A case pattern's variant written through its enum, Shape::Dot.
  void resolve_variant(sammine_util::QualifiedName &name,
                       sammine_util::Location location,
                       const ASTContext &where);

private:
  Scopes &scopes;
  const VariantEnums &variant_to_enum;
  sammine_util::Reportee &reportee;
  std::map<std::string, ImportAliases> module_aliases;
  //! Each top-level definition's module, and whether it exports it.
  struct Owner {
    std::string module;
    bool exported;
  };
  std::map<std::string, Owner> owners;

  //! Reports `name` if what it resolved to, or the enum of a variant it
  //! names, is private to a module other than `where.module`. A definition
  //! of a module that isn't exported can only be named by that module's own
  //! code, wherever it is compiled.
  void check_visible(const sammine_util::QualifiedName &name,
                     sammine_util::Location location, const ASTContext &where);

  //! What the first part of a qualified name means: one of the module's
  //! import aliases, rewritten to that module (m::f → math::f), or else a type
  //! of the module's own (Shape::Line → shapes::Shape::Line). False if it is
  //! neither, leaving `name` as written.
  bool resolve_head(sammine_util::QualifiedName &name, const ASTContext &where);
  //! The top-level definition of `where.module` that `name` means, if any.
  std::optional<sammine_util::QualifiedName>
  own_definition(const sammine_util::QualifiedName &name,
                 const ASTContext &where);
  //! Whether `name` is a local: a parameter or a variable, not a top-level
  //! definition.
  bool is_local(const std::string &name);
  bool is_visible(const std::string &name) const {
    return scopes.recursiveQueryName(name) == nameFound;
  }
};

} // namespace sammine_lang::AST
