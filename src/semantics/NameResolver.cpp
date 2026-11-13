#include "semantics/NameResolver.h"
#include "fmt/format.h"
#include "llvm/ADT/STLExtras.h"

//! \file NameResolver.cpp
//! \brief Implements NameResolver: what a name written in a module's code
//! means.
namespace sammine_lang::AST {

sammine_util::QualifiedName
NameResolver::qualify(const sammine_util::QualifiedName &name,
                      const std::string &module, bool exported) {
  if (name.is_qualified() || module.empty() || (module == kPrelude && exported))
    return name;
  return sammine_util::QualifiedName::from_parts(
      std::vector<sammine_util::QualifiedName::Part>{{module, ""},
                                                     name.parts().back()});
}

namespace {
/// `name`, written in code, as the definition named `defined` is named,
/// keeping the type arguments it was written with (m::id<i32>).
sammine_util::QualifiedName renamed(const sammine_util::QualifiedName &name,
                                    const sammine_util::QualifiedName &defined) {
  auto parts = defined.parts();
  parts.back() = name.parts().back();
  return sammine_util::QualifiedName::from_parts(std::move(parts));
}
} // namespace

void NameResolver::use_program(ProgramAST &program) {
  module_aliases = program.module_aliases;
  auto declare = [&](DefinitionAST &def, sammine_util::QualifiedName &n,
                     sammine_util::Location location) {
    auto &module = def.defining_module;
    auto written = n.strip_type_args().mangled();
    n = qualify(n, module, def.is_exported);
    // Defined twice in its module, or, in a program, as the prelude
    // exports it: a program's names and those are both unqualified.
    auto key = n.strip_type_args().mangled();
    if (auto it = definitions.find(key); it != definitions.end()) {
      reportee.add_error(
          location,
          fmt::format("[SCOPE]: The name `{}` has been introduced before",
                      written));
      reportee.add_error(
          it->second.location,
          fmt::format("[SCOPE]: Most recently defined `{}` is here", written));
      return;
    }
    top_levels[module].emplace(written, n.strip_type_args());
    definitions[key] = {module, def.is_exported, location};
  };
  for (auto &def : program.DefinitionVec) {
    // An instance's methods are code of its module.
    if (auto *tci = llvm::dyn_cast<TypeClassInstanceAST>(def.get()))
      for (auto &method : tci->methods)
        method->defining_module = def->defining_module;
    if (auto *fd = llvm::dyn_cast<FuncDefAST>(def.get()))
      declare(*def, fd->Prototype->functionName, fd->Prototype->get_location());
    else if (auto *kd = llvm::dyn_cast<KernelDefAST>(def.get()))
      declare(*def, kd->Prototype->functionName, kd->Prototype->get_location());
    else if (auto *ext = llvm::dyn_cast<ExternAST>(def.get())) {
      auto &fn = ext->Prototype->functionName;
      declare(*def, fn, ext->Prototype->get_location());
      if (ext->is_c_function)
        props.set_symbol(fn, fn.get_name());
    } else if (auto *sd = llvm::dyn_cast<StructDefAST>(def.get()))
      declare(*def, sd->struct_name, sd->get_location());
    else if (auto *ed = llvm::dyn_cast<EnumDefAST>(def.get()))
      declare(*def, ed->enum_name, ed->get_location());
    else if (auto *ta = llvm::dyn_cast<TypeAliasDefAST>(def.get()))
      declare(*def, ta->alias_name, ta->get_location());
  }
  // The prelude's names are in every module's scope: a library may not
  // take one it exports for its own either.
  auto prelude = top_levels.find(std::string(kPrelude));
  if (prelude == top_levels.end())
    return;
  for (auto &[module, table] : top_levels) {
    if (module == kPrelude)
      continue;
    for (auto &[written, name] : table) {
      auto it = prelude->second.find(written);
      if (it == prelude->second.end())
        continue;
      auto &theirs = definitions.at(it->second.mangled());
      if (!theirs.exported)
        continue;
      reportee.add_error(
          definitions.at(name.mangled()).location,
          fmt::format("[SCOPE]: The name `{}` has been introduced before",
                      written));
      reportee.add_error(
          theirs.location,
          fmt::format("[SCOPE]: Most recently defined `{}` is here", written));
    }
  }
}

std::optional<sammine_util::QualifiedName>
NameResolver::top_level(const std::string &module,
                        const std::string &name) const {
  auto own = top_levels.find(module);
  if (own != top_levels.end())
    if (auto it = own->second.find(name); it != own->second.end())
      return it->second;
  if (module == kPrelude)
    return std::nullopt;
  auto prelude = top_levels.find(std::string(kPrelude));
  if (prelude == top_levels.end())
    return std::nullopt;
  auto it = prelude->second.find(name);
  if (it == prelude->second.end() ||
      !definitions.at(it->second.mangled()).exported)
    return std::nullopt;
  return it->second;
}

bool NameResolver::is_local(const std::string &name) const {
  const auto *root = &scopes.root();
  for (const auto *scope = &scopes.top(); scope && scope != root;
       scope = scope->parent_scope)
    if (scope->queryName(name) == nameFound)
      return true;
  return false;
}

std::optional<sammine_util::Location>
NameResolver::defined_at(const std::string &name, const ASTContext &where) {
  if (is_local(name))
    return scopes.recursive_get_from_name(name);
  if (auto defined = top_level(where.module, name))
    return definitions.at(defined->mangled()).location;
  return std::nullopt;
}

bool NameResolver::resolve_head(sammine_util::QualifiedName &name,
                                const ASTContext &where) {
  auto parts = name.parts();
  if (auto aliases = module_aliases.find(where.module);
      aliases != module_aliases.end())
    if (auto it = aliases->second.find(parts.front().name);
        it != aliases->second.end()) {
      parts.front().name = it->second;
      name = sammine_util::QualifiedName::from_parts(std::move(parts));
      return true;
    }
  auto head = top_level(where.module, parts.front().name);
  if (!head)
    return false;
  auto resolved = head->parts();
  resolved.back() = parts.front();
  resolved.insert(resolved.end(), std::next(parts.begin()), parts.end());
  name = sammine_util::QualifiedName::from_parts(std::move(resolved));
  return true;
}

void NameResolver::resolve_type_name(sammine_util::QualifiedName &name,
                                     sammine_util::Location location,
                                     const ASTContext &where) {
  // A module's type, m::Point.
  if (name.is_qualified()) {
    if (!resolve_head(name, where))
      reportee.add_error(location, fmt::format("Module '{}' is not imported",
                                               name.get_module()));
    else
      check_visible(name, location, where);
    return;
  }
  if (llvm::is_contained(where.type_params, name.mangled()) ||
      is_builtin_type_name(name.get_name()))
    return;
  if (auto defined = top_level(where.module, name.strip_type_args().mangled()))
    name = renamed(name, *defined);
  check_visible(name, location, where);
}

void NameResolver::resolve_variant(sammine_util::QualifiedName &name,
                                   sammine_util::Location location,
                                   const ASTContext &where) {
  if (!name.is_qualified())
    return;
  resolve_head(name, where);
  check_visible(name, location, where);
}

void NameResolver::check_visible(const sammine_util::QualifiedName &name,
                                 sammine_util::Location location,
                                 const ASTContext &where) {
  auto plain = name.strip_type_args();
  for (auto &candidate : {plain.mangled(), plain.get_qualifier()}) {
    auto it = definitions.find(candidate);
    if (it == definitions.end())
      continue;
    auto &[module, exported, _] = it->second;
    if (!exported && !module.empty() && module != where.module)
      reportee.add_error(location, fmt::format("'{}' is private to module '{}'",
                                               candidate, module));
    return;
  }
}

void NameResolver::resolve_type(TypeExprAST *type, const ASTContext &where) {
  if (!type)
    return;
  if (auto *simple = llvm::dyn_cast<SimpleTypeExprAST>(type)) {
    resolve_type_name(simple->name, simple->location, where);
  } else if (auto *ptr = llvm::dyn_cast<PointerTypeExprAST>(type)) {
    resolve_type(ptr->pointee.get(), where);
  } else if (auto *arr = llvm::dyn_cast<ArrayTypeExprAST>(type)) {
    resolve_type(arr->element.get(), where);
  } else if (auto *gen = llvm::dyn_cast<GenericTypeExprAST>(type)) {
    resolve_type_name(gen->base_name, gen->location, where);
    for (auto &arg : gen->type_args)
      resolve_type(arg.get(), where);
  } else if (auto *func = llvm::dyn_cast<FunctionTypeExprAST>(type)) {
    for (auto &param : func->paramTypes)
      resolve_type(param.get(), where);
    resolve_type(func->returnType.get(), where);
  } else if (auto *tup = llvm::dyn_cast<TupleTypeExprAST>(type)) {
    for (auto &elem : tup->element_types)
      resolve_type(elem.get(), where);
  }
}

void NameResolver::resolve_value(sammine_util::QualifiedName &name,
                                 bool has_type_args, bool is_call,
                                 sammine_util::Location location,
                                 const ASTContext &where) {
  // A module's name, m::f, or a variant through its enum, Color::Red.
  if (name.is_qualified()) {
    auto written = name;
    bool head_known = resolve_head(name, where);
    check_visible(name, location, where);
    // A typeclass method or a generic enum's variant: the type checker
    // resolves it.
    if (has_type_args)
      return;
    if (!head_known) {
      reportee.add_error(location, fmt::format("Module '{}' is not imported",
                                               name.get_module()));
      return;
    }
    auto plain = name.strip_type_args();
    if (definitions.contains(plain.mangled()) ||
        definitions.contains(plain.get_qualifier()))
      return;
    reportee.add_error(
        location, is_call
                      ? fmt::format("[SCOPE]: The called name {} for the call "
                                    "expression is not found before",
                                    name.mangled())
                      : fmt::format("The name {} is not found before",
                                    written.mangled()));
    return;
  }

  auto bare = name.mangled();
  if (is_local(bare))
    return;
  if (auto defined = top_level(where.module, name.strip_type_args().mangled())) {
    name = renamed(name, *defined);
    check_visible(name, location, where);
    return;
  }

  // A variant written alone, Red: the enum of the module's own code first.
  if (auto it = variant_to_enum.find(bare); it != variant_to_enum.end()) {
    std::vector<sammine_util::QualifiedName> enums;
    for (auto &e : it->second)
      if (!where.module.empty() && e.get_module() == where.module)
        enums.push_back(e);
    if (enums.empty())
      for (auto &e : it->second)
        if (where.module.empty() || !e.is_qualified())
          enums.push_back(e);
    if (enums.size() > 1) {
      reportee.add_error(
          location, fmt::format("[SCOPE]: Variant name '{}' is ambiguous — "
                                "multiple enums define this variant. Use "
                                "qualified syntax (e.g., EnumName::{}) to "
                                "disambiguate",
                                bare, bare));
      return;
    }
    if (enums.size() == 1) {
      name = enums.front().with_appended({bare, ""});
      check_visible(name, location, where);
      return;
    }
  }

  if (has_type_args)
    return;
  reportee.add_error(
      location, is_call
                    ? fmt::format("[SCOPE]: The called name {} for the call "
                                  "expression is not found before",
                                  bare)
                    : fmt::format("The variable named {} for the variable "
                                  "expression is not found before",
                                  bare));
}

} // namespace sammine_lang::AST
