#pragma once
#include "ast/ASTContext.h"
#include "ast/ASTProperties.h"
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
//! only read names. It also decides the symbol each definition is linked as,
//! in ASTProperties: its name, but a C binding's is its C name, as a C
//! function is bound in many modules (std::printf, io::printf), each with
//! the types it gives it, and C knows it as one (printf).
//!
//! Every module has its own scope, wherever its code is compiled: a name
//! written bare in module M's code is a local, else one of M's own top-level
//! definitions, else one the prelude exports, and nothing else. An imported
//! generic function, compiled in the importer, still sees only its module's
//! names: the importer's can't capture its names, or clash with its
//! parameters. A qualified name, m::f, goes through M's import aliases.
//!
//! Locals are the scope generator's scopes; its table of variants to enums
//! is its too.
class NameResolver {
public:
  using Scopes = LexicalStack<sammine_util::Location>;
  using VariantEnums =
      std::map<std::string, std::vector<sammine_util::QualifiedName>>;

  NameResolver(Scopes &scopes, const VariantEnums &variant_to_enum,
               sammine_util::Reportee &reportee, ASTProperties &props)
      : scopes(scopes), variant_to_enum(variant_to_enum), reportee(reportee),
        props(props) {}

  //! Name every definition of `program`, give each C binding its symbol,
  //! take its modules' import aliases, and give each module its top-level
  //! names, before any name is resolved. A module defining a name twice, or
  //! one the prelude exports, is reported.
  void use_program(ProgramAST &program);

  //! The prelude, stdlib/definitions.mn: every other module imports it
  //! without writing `import`, as Rust's prelude is, and its names are used
  //! without a prefix (Option, not definitions::Option), everywhere,
  //! including in its own library, so symbols built from them agree.
  static constexpr std::string_view kPrelude = "definitions";

  //! `name` as a definition of `module` is named: module::name. A name
  //! already qualified, of no module (an executable's) or exported by the
  //! prelude, is unchanged. The prelude's private names are qualified as any
  //! module's: they aren't to be used, or taken, without a prefix elsewhere.
  static sammine_util::QualifiedName
  qualify(const sammine_util::QualifiedName &name, const std::string &module,
          bool exported = true);

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

  //! Where `name`, written bare in `where.module`'s code, is already
  //! defined, if it is: a local, or a top-level name in scope there.
  std::optional<sammine_util::Location>
  defined_at(const std::string &name, const ASTContext &where);

private:
  Scopes &scopes;
  const VariantEnums &variant_to_enum;
  sammine_util::Reportee &reportee;
  ASTProperties &props;
  std::map<std::string, ImportAliases> module_aliases;
  //! Each top-level definition, by its name (m::f): its module, whether it
  //! exports it, and where it is.
  struct Definition {
    std::string module;
    bool exported;
    sammine_util::Location location;
  };
  std::map<std::string, Definition> definitions;
  //! Per module, its top-level definitions' names as its code writes them
  //! (f), each to its name (m::f).
  std::map<std::string, std::map<std::string, sammine_util::QualifiedName>>
      top_levels;

  //! The name of the top-level definition `name` means, written bare in
  //! `module`'s code: one of the module's own, or one the prelude exports.
  std::optional<sammine_util::QualifiedName>
  top_level(const std::string &module, const std::string &name) const;
  //! The name of what the prelude exports as `name`, if it does.
  std::optional<sammine_util::QualifiedName>
  prelude_export(const std::string &name) const;
  //! Reports `written` defined at `here` when it was already, at `before`.
  void report_redefinition(const std::string &written,
                           sammine_util::Location here,
                           sammine_util::Location before);
  //! Whether `name` is a local: a parameter or a variable, not a top-level
  //! definition.
  bool is_local(const std::string &name) const;

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
};

} // namespace sammine_lang::AST
