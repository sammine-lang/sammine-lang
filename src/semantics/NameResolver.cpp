#include "semantics/NameResolver.h"
#include "fmt/format.h"
#include "llvm/ADT/STLExtras.h"

//! \file NameResolver.cpp
//! \brief Implements NameResolver: what a name written in a module's code
//! means.
namespace sammine_lang::AST {

sammine_util::QualifiedName
NameResolver::qualify(const sammine_util::QualifiedName &name,
                      const std::string &module) {
  if (name.is_qualified() || module.empty())
    return name;
  return sammine_util::QualifiedName::from_parts(
      std::vector<sammine_util::QualifiedName::Part>{{module, ""},
                                                     name.parts().back()});
}

void NameResolver::use_program(ProgramAST &program) {
  module_aliases = program.module_aliases;
  for (auto &def : program.DefinitionVec) {
    auto &module = def->defining_module;
    auto name = [&](sammine_util::QualifiedName &n) {
      n = qualify(n, module);
      owners[n.strip_type_args().mangled()] = {module, def->is_exported};
    };
    if (auto *fd = llvm::dyn_cast<FuncDefAST>(def.get()))
      name(fd->Prototype->functionName);
    else if (auto *kd = llvm::dyn_cast<KernelDefAST>(def.get()))
      name(kd->Prototype->functionName);
    else if (auto *ext = llvm::dyn_cast<ExternAST>(def.get())) {
      // A module's own C binding is named as C names it: that is its symbol.
      if (!(ext->is_c_function && module == program.module))
        name(ext->Prototype->functionName);
    } else if (auto *sd = llvm::dyn_cast<StructDefAST>(def.get()))
      name(sd->struct_name);
    else if (auto *ed = llvm::dyn_cast<EnumDefAST>(def.get()))
      name(ed->enum_name);
    else if (auto *ta = llvm::dyn_cast<TypeAliasDefAST>(def.get()))
      name(ta->alias_name);
  }
}

std::optional<sammine_util::QualifiedName>
NameResolver::own_definition(const sammine_util::QualifiedName &name,
                             const ASTContext &where) {
  if (name.is_qualified() || where.module.empty())
    return std::nullopt;
  auto qualified = qualify(name, where.module);
  if (scopes.root().queryName(qualified.strip_type_args().mangled()) !=
      nameFound)
    return std::nullopt;
  return qualified;
}

bool NameResolver::is_local(const std::string &name) {
  const auto *root = &scopes.root();
  for (const auto *scope = &scopes.top(); scope && scope != root;
       scope = scope->parent_scope)
    if (scope->queryName(name) == nameFound)
      return true;
  return false;
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
  auto head = sammine_util::QualifiedName::from_parts(
      std::vector<sammine_util::QualifiedName::Part>{parts.front()});
  if (auto own = own_definition(head, where)) {
    auto resolved = own->parts();
    resolved.insert(resolved.end(), std::next(parts.begin()), parts.end());
    name = sammine_util::QualifiedName::from_parts(std::move(resolved));
    return true;
  }
  return false;
}

void NameResolver::resolve_type_name(sammine_util::QualifiedName &name,
                                     sammine_util::Location location,
                                     const ASTContext &where) {
  // A module's type, m::Point.
  if (name.is_qualified()) {
    if (!resolve_head(name, where) && !is_visible(name.get_module()))
      reportee.add_error(location, fmt::format("Module '{}' is not imported",
                                               name.get_module()));
    else
      check_visible(name, location, where);
    return;
  }
  if (llvm::is_contained(where.type_params, name.mangled()) ||
      is_builtin_type_name(name.get_name()))
    return;
  if (auto own = own_definition(name, where))
    name = *own;
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
    auto it = owners.find(candidate);
    if (it == owners.end())
      continue;
    auto &[module, exported] = it->second;
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
    if (!head_known && !is_visible(name.get_module())) {
      reportee.add_error(location, fmt::format("Module '{}' is not imported",
                                               name.get_module()));
      return;
    }
    if (is_visible(name.mangled()) || is_visible(name.get_qualifier()))
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
  if (auto own = own_definition(name, where)) {
    name = *own;
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

  if (has_type_args || is_visible(bare))
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
