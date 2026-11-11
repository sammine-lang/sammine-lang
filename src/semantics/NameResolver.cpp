#include "semantics/NameResolver.h"
#include "fmt/format.h"
#include "llvm/ADT/STLExtras.h"

//! \file NameResolver.cpp
//! \brief Implements NameResolver: what a name written in a module's code
//! means.
namespace sammine_lang::AST {

std::optional<sammine_util::QualifiedName>
NameResolver::own_definition(const sammine_util::QualifiedName &name,
                             const ASTContext &where) {
  if (name.is_qualified() || where.module.empty())
    return std::nullopt;
  auto qualified = name.with_module(where.module);
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

void NameResolver::resolve_type_name(sammine_util::QualifiedName &name,
                                     const ASTContext &where) {
  if (llvm::is_contained(where.type_params, name.mangled()) ||
      is_builtin_type_name(name.get_name()))
    return;
  if (auto own = own_definition(name, where))
    name = *own;
}

void NameResolver::resolve_type(TypeExprAST *type, const ASTContext &where) {
  if (!type)
    return;
  if (auto *simple = llvm::dyn_cast<SimpleTypeExprAST>(type)) {
    resolve_type_name(simple->name, where);
  } else if (auto *ptr = llvm::dyn_cast<PointerTypeExprAST>(type)) {
    resolve_type(ptr->pointee.get(), where);
  } else if (auto *arr = llvm::dyn_cast<ArrayTypeExprAST>(type)) {
    resolve_type(arr->element.get(), where);
  } else if (auto *gen = llvm::dyn_cast<GenericTypeExprAST>(type)) {
    resolve_type_name(gen->base_name, where);
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
  // X::N where X isn't an imported module: X is a type, e.g. the enum of the
  // variant Shape::Line, resolved as a type is.
  if (name.is_unresolved()) {
    auto parts = name.parts();
    auto head = sammine_util::QualifiedName::from_parts(
        std::vector<sammine_util::QualifiedName::Part>{parts.front()});
    if (auto own = own_definition(head, where)) {
      auto resolved = own->parts();
      resolved.insert(resolved.end(), std::next(parts.begin()), parts.end());
      name = sammine_util::QualifiedName::from_parts(std::move(resolved));
      return;
    }
    if (!has_type_args && !is_visible(name.get_module()))
      reportee.add_error(location, fmt::format("Module '{}' is not imported",
                                               name.get_module()));
    return;
  }

  // An import's name, m::f, or an enum's variant, Color::Red.
  if (name.is_qualified()) {
    if (has_type_args || is_visible(name.mangled()) ||
        is_visible(name.get_qualifier()))
      return;
    reportee.add_error(
        location, is_call
                      ? fmt::format("[SCOPE]: The called name {} for the call "
                                    "expression is not found before",
                                    name.mangled())
                      : fmt::format("The name {} is not found before",
                                    name.with_alias().mangled()));
    return;
  }

  auto bare = name.mangled();
  if (is_local(bare))
    return;
  if (auto own = own_definition(name, where)) {
    name = *own;
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
