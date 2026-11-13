#include "typecheck/BiTypeChecker.h"
#include "ast/Ast.h"
#include "typecheck/BuiltinInstances.h"
#include "typecheck/UniSubst.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "util/Utilities.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

#define DEBUG_TYPE "typecheck"
#include "util/Logging.h"

//! \file BiTypeChecker.cpp
//! \brief Bidirectional type checker. Uses two operations:
//!   - synthesize(): infer a type bottom-up from an expression (in
//!   BiTypeCheckerSynthesize.cpp)
//!   - check/visit(): validate against an expected type top-down
//! Three-pass approach over ProgramAST:
//!   1) Register all type definitions (structs, enums, typeclasses, aliases)
//!   2) Pre-register all function signatures (enables mutual recursion)
//!   3) Full type checking of function bodies + expressions
namespace sammine_lang::AST {

// visit overrides — explicit traversal order

void BiTypeCheckerVisitor::pre_register_function(PrototypeAST *ast) {
  // Build the function type by resolving type expressions directly,
  // without calling accept_synthesis (which would mark TypedVarASTs
  // as synthesized and prevent parameter re-registration in the full pass).
  std::vector<Type> types;
  for (auto &param : ast->parameterVectors)
    types.push_back(resolve_type_expr(param->type_expr.get()));
  if (ast->returnsUnit())
    types.push_back(Type::Unit());
  else
    types.push_back(resolve_type_expr(ast->return_type_expr.get()));
  auto fn_type = Type::Function(std::move(types), ast->is_var_arg);
  register_function(ast->functionName.mangled(), std::move(fn_type));
}

void BiTypeCheckerVisitor::visit(ProgramAST *ast) {
  top_level_ast = ast;

  // First pass: register every type, typeclass and instance before checking
  // any code. As with functions, every type is declared before any is
  // defined, so a type can use one declared after it.
  // 1) Declare each struct and enum; register each generic one.
  for (auto &def : ast->DefinitionVec) {
    if (auto *sd = llvm::dyn_cast<StructDefAST>(def.get())) {
      if (sd->type_params.empty())
        declare_struct(sd);
      else
        sd->accept_vis(this);
    } else if (auto *ed = llvm::dyn_cast<EnumDefAST>(def.get())) {
      if (ed->type_params.empty())
        declare_enum(ed);
      else
        ed->accept_vis(this);
    }
  }
  // 2) Resolve type aliases. Like rustc, an alias is expanded when first
  //    needed, so it can name an alias declared after it
  //    (resolve_pending_alias).
  for (auto &def : ast->DefinitionVec)
    if (auto *ta = llvm::dyn_cast<TypeAliasDefAST>(def.get()))
      pending_aliases[ta->alias_name.mangled()] = ta;
  for (auto &def : ast->DefinitionVec)
    if (auto *ta = llvm::dyn_cast<TypeAliasDefAST>(def.get()))
      ta->accept_vis(this);
  // 3) Fill in each struct's fields and each enum's variants.
  for (auto &def : ast->DefinitionVec)
    if (llvm::isa<StructDefAST, EnumDefAST>(def.get()))
      def->accept_vis(this);
  // 4) An alias of an enum names its variants too, now that they are known.
  for (auto &def : ast->DefinitionVec)
    if (auto *ta = llvm::dyn_cast<TypeAliasDefAST>(def.get()))
      register_alias_variants(ta);
  // 5) Typeclasses, then their instances.
  for (auto &def : ast->DefinitionVec)
    if (auto *tc = llvm::dyn_cast<TypeClassDeclAST>(def.get()))
      register_typeclass_decl(tc);
  for (auto &def : ast->DefinitionVec)
    if (auto *tci = llvm::dyn_cast<TypeClassInstanceAST>(def.get()))
      register_typeclass_instance(tci);

  // Second pass: pre-register all function signatures so mutual recursion works
  for (auto &def : ast->DefinitionVec) {
    if (auto *fd = llvm::dyn_cast<FuncDefAST>(def.get())) {
      if (fd->Prototype->is_generic())
        monomorphizer.generic_funcs.register_def(
            fd->Prototype->functionName.mangled(), fd);
      else
        pre_register_function(fd->Prototype.get());
    } else if (auto *ext = llvm::dyn_cast<ExternAST>(def.get())) {
      if (!ext->Prototype->is_generic())
        pre_register_function(ext->Prototype.get());
    } else if (auto *tci = llvm::dyn_cast<TypeClassInstanceAST>(def.get())) {
      // A generic instance's methods are registered with each copy.
      if (!tci->is_generic())
        for (auto &method : tci->methods)
          if (!method->Prototype->is_generic())
            pre_register_function(method->Prototype.get());
    } else if (auto *kd = llvm::dyn_cast<KernelDefAST>(def.get())) {
      if (!kd->Prototype->is_generic())
        pre_register_function(kd->Prototype.get());
    }
  }

  // Third pass: full type checking of everything
  for (auto &def : ast->DefinitionVec)
    def->accept_vis(this);

  instantiate_drops();
}

// Each type a generic instantiates gets its Drop, if a generic instance
// gives it one: no source calls it, the drops MIR inserts do. The copies may
// instantiate more types, which get theirs too.
void BiTypeCheckerVisitor::instantiate_drops() {
  auto drop = [&](const sammine_util::QualifiedName &name) {
    auto type = get_typename_type(name.mangled());
    if (!type)
      return;
    MonomorphizedKey key{std::string(kDropClass), {*type}};
    if (!type_class_instances.contains(key))
      instantiate_generic_instance(key);
  };
  for (size_t s = 0, e = 0;
       s < monomorphizer.monomorphized_struct_defs.size() ||
       e < monomorphizer.monomorphized_enum_defs.size();) {
    if (s < monomorphizer.monomorphized_struct_defs.size())
      drop(monomorphizer.monomorphized_struct_defs[s++]->struct_name);
    else
      drop(monomorphizer.monomorphized_enum_defs[e++]->enum_name);
  }
}

void BiTypeCheckerVisitor::visit(FuncDefAST *ast) {
  ScopeGuard scope(*this);
  ctx().enclosing_function = ast;

  if (ast->Prototype->is_var_arg) {
    this->add_error(
        ast->Prototype->get_location(),
        "Variadic arguments ('...') are only supported on extern declarations "
        "for C interop, not on function definitions");
  }

  // Register explicitly-declared type parameters before visiting the prototype
  for (auto &tp_name : ast->Prototype->type_params) {
    auto tp = Type::TypeParam(tp_name);
    typename_to_type.registerNameT(tp_name, tp);
  }

  // Visit prototype (resolves param/return types, registers function name)
  // Previously done inside discover_type_params().
  ast->Prototype->accept_vis(this);
  ast->accept_synthesis(this);

  if (ast->Prototype->is_generic()) {
    monomorphizer.generic_funcs.register_def(
        ast->Prototype->functionName.mangled(), ast);
    return;
  }
  ast->Block->accept_vis(this);
  // A body that runs off its end returns its value; one ending in a
  // statement returns (), which must be what the function returns.
  if (ast->returnsUnit() || ast->Block->get_type().is_never())
    return;
  if (auto *value = ast->body_value())
    check_returned(value, value->get_type(), value->get_location());
  else
    check_returned(nullptr, Type::Unit(), ast->Block->get_location());
}

void BiTypeCheckerVisitor::visit(PrototypeAST *ast) {
  ast->accept_synthesis(this);
  for (auto &var : ast->parameterVectors)
    var->accept_vis(this);
  register_function(ast->functionName.mangled(), ast->get_type());
  if (ast->functionName.get_name() == "main") {
    auto fn_type = std::get<FunctionType>(ast->get_type().type_data);
    auto return_type = fn_type.get_return_type();
    if (return_type != Type::I32_t()) {
      this->add_error(ast->get_location(),
                      fmt::format("main must return i32, found {}",
                                  return_type.to_string()));
    }
    // Validate parameters: either 0 args or (i32, ptr<ptr<char>>)
    auto param_count = ast->parameterVectors.size();
    if (param_count != 0 && param_count != 2) {
      this->add_error(
          ast->get_location(),
          "main must take 0 or 2 parameters (argc: i32, argv: ptr<ptr<char>>)");
    }
    if (param_count == 2) {
      auto argc_type = ast->parameterVectors[0]->get_type();
      auto argv_type = ast->parameterVectors[1]->get_type();
      if (argc_type != Type::I32_t()) {
        this->add_error(ast->parameterVectors[0]->get_location(),
                        fmt::format("main's first parameter must be i32 "
                                    "(argc), found {}",
                                    argc_type.to_string()));
      }
      if (argv_type != Type::Pointer(Type::Pointer(Type::Char()))) {
        this->add_error(ast->parameterVectors[1]->get_location(),
                        fmt::format("main's second parameter must be "
                                    "ptr<ptr<char>> (argv), found {}",
                                    argv_type.to_string()));
      }
    }
  }
}

void BiTypeCheckerVisitor::visit(VarDefAST *ast) {
  // Tuple destructuring: let (a, b) = expr;
  if (ast->is_tuple_destructure) {
    ast->Expression->accept_vis(this);
    ast->accept_synthesis(this);
    return;
  }

  // Normal case
  ast->Expression->accept_vis(this);
  ast->accept_synthesis(this);
  auto to = ast->get_type();
  auto from = ast->Expression->accept_synthesis(this);
  if (to == Type::Poisoned() || from == Type::Poisoned()) {
    ast->set_type(Type::Poisoned());
  } else if (!type_map_ordering.compatible_to_from(to, from)) {
    report_type_mismatch(ast->Expression->get_location(),
                         fmt::format("Type mismatch: expression has type {}, "
                                     "but variable declared as {}",
                                     from.to_string(), to.to_string()),
                         to, from);
    ast->set_type(Type::Poisoned());
  } else {
    resolve_literal_type(ast->Expression.get(), to);
  }
}

// TODO: calling a reuse function should be allowed only in an unsafe
// function, like indexing a pointer (see check_unsafe_allowed): nothing checks
// what the C code does. Nearly every program calls std::printf, so this waits
// until printf is reworked.
void BiTypeCheckerVisitor::visit(ExternAST *ast) {
  ScopeGuard scope(*this);
  if (ast->Prototype->is_generic()) {
    this->add_error(
        ast->Prototype->get_location(),
        "Generic reuse declarations are not supported; generics require a "
        "function body for monomorphization");
    return;
  }
  ast->Prototype->accept_vis(this);
}

void BiTypeCheckerVisitor::visit(StructDefAST *ast) {
  // Generic struct: register as template, skip concrete registration
  if (!ast->type_params.empty()) {
    if (!ast->get_type().synthesized()) {
      monomorphizer.generic_structs.register_def(ast->struct_name.mangled(),
                                                 ast);
      ast->set_type(Type::Generic()); // mark as visited (not concretely usable)
    }
    return;
  }

  auto type = declare_struct(ast);
  auto &st = std::get<StructType>(type.type_data);
  if (st.is_defined())
    return;

  std::vector<std::string> field_names;
  std::vector<Type> field_types;
  for (auto &member : ast->struct_members) {
    auto ft = resolve_type_expr(member->type_expr.get());
    if (refers_to_itself(member->type_expr.get(), ft, type))
      ft = Type::Poisoned();
    field_names.push_back(member->name);
    field_types.push_back(ft);
  }
  st.define(std::move(field_names), std::move(field_types));
}

Type BiTypeCheckerVisitor::declare_struct(StructDefAST *ast) {
  if (ast->get_type().type_kind == TypeKind::Struct)
    return ast->get_type();
  auto type = Type::DeclaredStruct(ast->struct_name, ast->instance_of);
  ast->set_type(type);
  typename_to_type.registerNameTAtRoot(ast->struct_name.mangled(), type);
  return type;
}

Type BiTypeCheckerVisitor::declare_enum(EnumDefAST *ast) {
  if (ast->get_type().type_kind == TypeKind::Enum)
    return ast->get_type();
  auto type = Type::DeclaredEnum(ast->enum_name, ast->instance_of);
  ast->set_type(type);
  typename_to_type.registerNameTAtRoot(ast->enum_name.mangled(), type);
  return type;
}

// Whether `type` contains or points to `self`, through any pointer, array,
// tuple, function, struct field or enum payload. Reported at `where`.
// sammine doesn't allow a type that refers to itself.
bool BiTypeCheckerVisitor::refers_to_itself(TypeExprAST *where,
                                            const Type &type,
                                            const Type &self) {
  std::unordered_set<std::string> seen;
  std::function<bool(const Type &)> reaches = [&](const Type &t) -> bool {
    switch (t.type_kind) {
    case TypeKind::Pointer:
      return reaches(std::get<PointerType>(t.type_data).get_pointee());
    case TypeKind::Array:
      return reaches(std::get<ArrayType>(t.type_data).get_element());
    case TypeKind::Tuple:
      return llvm::any_of(
          std::get<TupleType>(t.type_data).get_element_types(), reaches);
    case TypeKind::Function: {
      auto &ft = std::get<FunctionType>(t.type_data);
      return llvm::any_of(ft.get_params_types(), reaches) ||
             reaches(ft.get_return_type());
    }
    case TypeKind::Struct: {
      if (t == self)
        return true;
      auto &st = std::get<StructType>(t.type_data);
      return seen.insert(st.get_name().mangled()).second &&
             llvm::any_of(st.get_field_types(), reaches);
    }
    case TypeKind::Enum: {
      if (t == self)
        return true;
      auto &et = std::get<EnumType>(t.type_data);
      return seen.insert(et.get_name().mangled()).second &&
             llvm::any_of(et.get_variants(), [&](auto &vi) {
               return llvm::any_of(vi.payload_types, reaches);
             });
    }
    default:
      return false;
    }
  };
  if (!reaches(type))
    return false;
  this->add_error(where->location,
                  fmt::format("'{}' refers to itself here, and a type can't "
                              "refer to itself",
                              self.to_string()));
  return true;
}

void BiTypeCheckerVisitor::visit(EnumDefAST *ast) {
  // Generic enum: register as template, skip concrete registration
  if (!ast->type_params.empty()) {
    if (!ast->get_type().synthesized()) {
      monomorphizer.generic_enums.register_def(ast->enum_name.mangled(), ast);
      ast->set_type(Type::Generic()); // mark as visited (not concretely usable)
    }
    return;
  }

  auto enum_type = declare_enum(ast);
  auto &et = std::get<EnumType>(enum_type.type_data);
  if (et.is_defined())
    return;
  // A variant list that fails a check below is left empty, so the error is
  // reported once.
  auto fail = [&] { et.define({}, false, TypeKind::I32_t); };

  // Resolve backing type if specified
  TypeKind backing_type = TypeKind::I32_t;
  if (ast->backing_type_name.has_value()) {
    static const std::unordered_map<std::string, TypeKind> backing_type_map = {
        {"i32", TypeKind::I32_t},
        {"i64", TypeKind::I64_t},
        {"u32", TypeKind::U32_t},
        {"u64", TypeKind::U64_t},
    };
    auto &bt_name = ast->backing_type_name.value();
    auto it = backing_type_map.find(bt_name);
    if (it != backing_type_map.end()) {
      backing_type = it->second;
    } else {
      this->add_error(
          ast->get_location(),
          fmt::format("Invalid backing type '{}' for type '{}' — must be "
                      "i32, i64, u32, or u64",
                      bt_name, ast->enum_name.mangled()));
      fail();
      return;
    }
    // Backing type annotation implies integer-backed
    ast->is_integer_backed = true;
  }

  // Validate integer-backed enums: all variants must have discriminant values,
  // no mixing with type payloads
  if (ast->is_integer_backed) {
    std::set<int64_t> seen_values;
    for (auto &variant : ast->variants) {
      if (!variant.discriminant_value.has_value()) {
        this->add_error(
            variant.location,
            fmt::format("Integer-backed enum '{}': variant '{}' is missing a "
                        "discriminant value — all variants must have one",
                        ast->enum_name.mangled(), variant.name));
        fail();
        return;
      }
      if (!variant.payload_types.empty()) {
        this->add_error(
            variant.location,
            fmt::format("Integer-backed enum '{}': variant '{}' cannot have "
                        "both a discriminant value and type payloads",
                        ast->enum_name.mangled(), variant.name));
        fail();
        return;
      }
      auto val = variant.discriminant_value.value();
      if (seen_values.contains(val)) {
        this->add_error(
            variant.location,
            fmt::format("Integer-backed enum '{}': duplicate discriminant "
                        "value {} on variant '{}'",
                        ast->enum_name.mangled(), val, variant.name));
        fail();
        return;
      }
      seen_values.insert(val);
    }
  }

  std::vector<EnumType::VariantInfo> variant_infos;

  for (auto &variant : ast->variants) {
    EnumType::VariantInfo info;
    info.name = variant.name;
    info.discriminant_value = variant.discriminant_value;
    for (auto &type_expr : variant.payload_types) {
      auto resolved = resolve_type_expr(type_expr.get());
      if (refers_to_itself(type_expr.get(), resolved, enum_type))
        resolved = Type::Poisoned();
      info.payload_types.push_back(resolved);
    }
    variant_infos.push_back(std::move(info));
  }

  et.define(std::move(variant_infos), ast->is_integer_backed, backing_type);

  // Add lattice edge: integer-backed enum → backing type
  if (ast->is_integer_backed) {
    auto backing = [&]() -> Type {
      switch (backing_type) {
      case TypeKind::I32_t:
        return Type::I32_t();
      case TypeKind::I64_t:
        return Type::I64_t();
      case TypeKind::U32_t:
        return Type::U32_t();
      case TypeKind::U64_t:
        return Type::U64_t();
      default:
        return Type::I32_t();
      }
    }();
    type_map_ordering.type_map[enum_type] = backing;
  }

  // Register variant constructors with qualified keys (enum_name::variant_name)
  // so lookups match the fully-qualified names produced by scope generator.
  // A generic enum's Option::Some names no one instance: enum_of_variant
  // infers which.
  for (size_t i = 0; i < et.variant_count(); i++) {
    auto &vi = et.get_variant(i);
    auto qualified_key = ast->enum_name.with_appended({vi.name, ""}).mangled();
    variant_constructors[qualified_key] = {enum_type, i};
  }
}

void BiTypeCheckerVisitor::visit(TypeAliasDefAST *ast) {
  // Already resolved — skip on third pass
  auto &tap = props_.type_alias(ast->id());
  if (tap.resolved_type.synthesized())
    return;
  auto name = ast->alias_name.mangled();

  alias_path.push_back(ast);
  auto resolved = resolve_type_expr(ast->type_expr.get());
  alias_path.pop_back();
  pending_aliases.erase(name);
  // Part of a cycle, already reported.
  if (tap.resolved_type.synthesized())
    return;
  if (resolved.is_poisoned()) {
    this->add_error(ast->get_location(),
                    fmt::format("Cannot resolve type '{}' in type alias '{}'",
                                ast->type_expr->to_string(), name));
    tap.resolved_type = Type::Poisoned();
    ast->set_type(Type::Poisoned());
    return;
  }
  tap.resolved_type = resolved;
  ast->set_type(resolved);
  typename_to_type.registerNameTAtRoot(name, resolved);
}

std::optional<Type>
BiTypeCheckerVisitor::resolve_pending_alias(const std::string &name) {
  auto it = pending_aliases.find(name);
  if (it == pending_aliases.end())
    return std::nullopt;
  auto *alias = it->second;

  // Already being expanded: the alias refers to itself through the aliases
  // expanded since.
  auto start = llvm::find(alias_path, alias);
  if (start != alias_path.end()) {
    std::vector<std::string> through;
    for (auto at = std::next(start); at != alias_path.end(); ++at)
      through.push_back(fmt::format("'{}'", (*at)->alias_name.mangled()));
    this->add_error(
        alias->get_location(),
        through.empty()
            ? fmt::format("Type alias '{}' refers to itself", name)
            : fmt::format("Type alias '{}' refers to itself through {}", name,
                          fmt::join(through, ", ")));
    // Every alias in the cycle is unresolvable; nothing else reports it.
    for (auto at = start; at != alias_path.end(); ++at) {
      props_.type_alias((*at)->id()).resolved_type = Type::Poisoned();
      (*at)->set_type(Type::Poisoned());
      typename_to_type.registerNameTAtRoot((*at)->alias_name.mangled(),
                                           Type::Poisoned());
    }
    return Type::Poisoned();
  }

  alias->accept_vis(this);
  return get_typename_type(name);
}

void BiTypeCheckerVisitor::register_alias_variants(TypeAliasDefAST *ast) {
  auto resolved = props_.type_alias(ast->id()).resolved_type;
  if (resolved.type_kind == TypeKind::Enum) {
    auto &et = std::get<EnumType>(resolved.type_data);
    for (size_t i = 0; i < et.variant_count(); i++) {
      auto &vi = et.get_variant(i);
      auto qualified_key =
          ast->alias_name.with_appended({vi.name, ""}).mangled();
      variant_constructors[qualified_key] = {resolved, i};
    }
  }
}

const std::vector<std::string> *BiTypeCheckerVisitor::generic_type_params(
    const sammine_util::QualifiedName &base) const {
  if (auto *def = monomorphizer.generic_enums.find(base.mangled()))
    return &def->type_params;
  if (auto *def = monomorphizer.generic_structs.find(base.mangled()))
    return &def->type_params;
  return nullptr;
}

Type BiTypeCheckerVisitor::generic_type_instance(
    const sammine_util::QualifiedName &base, std::vector<Type> type_args) {
  auto *enum_def = monomorphizer.generic_enums.find(base.mangled());
  auto *struct_def =
      enum_def ? nullptr : monomorphizer.generic_structs.find(base.mangled());
  if (!enum_def && !struct_def)
    return Type::Poisoned();
  auto &type_params = enum_def ? enum_def->type_params : struct_def->type_params;

  TypeBindings bindings;
  bool has_unresolved_type_param = false;
  for (size_t i = 0; i < type_args.size(); i++) {
    if (type_args[i].containsTypeParam() || type_args[i].is_open())
      has_unresolved_type_param = true;
    bindings[type_params[i]] = type_args[i];
  }
  MonomorphizedKey key{base.mangled(), std::move(type_args)};
  auto mangled = key.to_generic_name(base).mangled();

  // An argument that is still a type parameter, as Vec<T> inside a generic
  // signature, or still open, as in None's Option<_>, can't be instantiated
  // yet: a placeholder with it in its fields stands in, until substitute()
  // or the context decides it.
  if (has_unresolved_type_param) {
    if (auto existing = get_typename_type(mangled))
      return *existing;

    auto instance_of = std::make_shared<const MonomorphizedKey>(key);
    Type placeholder = Type::Poisoned();
    {
      // The generic's own parameters, as its fields name them.
      decltype(typename_to_type)::Guard scope(typename_to_type);
      for (auto &[pname, ptype] : bindings)
        typename_to_type.registerNameT(pname, ptype);

      auto name = sammine_util::QualifiedName::from_parts({mangled});
      if (struct_def) {
        std::vector<std::string> fnames;
        std::vector<Type> ftypes;
        for (auto &member : struct_def->struct_members) {
          fnames.push_back(member->name);
          ftypes.push_back(member->type_expr
                               ? resolve_type_expr(member->type_expr.get())
                               : Type::Poisoned());
        }
        placeholder = Type::Struct(name, std::move(fnames), std::move(ftypes),
                                   std::move(instance_of));
      } else {
        std::vector<EnumType::VariantInfo> variant_infos;
        for (auto &v : enum_def->variants) {
          std::vector<Type> payload;
          for (auto &pt : v.payload_types)
            payload.push_back(resolve_type_expr(pt.get()));
          variant_infos.push_back(
              {v.name, std::move(payload), v.discriminant_value});
        }
        placeholder = Type::Enum(name, std::move(variant_infos), false,
                                 TypeKind::I32_t, std::move(instance_of));
      }
    }
    typename_to_type.registerNameT(mangled, placeholder);
    return placeholder;
  }

  // An instance is registered at the root scope once instantiated.
  if (auto existing = get_typename_type(mangled))
    return *existing;
  if (enum_def)
    monomorphizer.instantiate_enum(enum_def, key, bindings)->accept_vis(this);
  else
    monomorphizer.instantiate_struct(struct_def, key, bindings)
        ->accept_vis(this);
  auto result = get_typename_type(mangled);
  return result.has_value() ? *result : Type::Poisoned();
}

std::optional<Type> BiTypeCheckerVisitor::common_type(const Type &a,
                                                      const Type &b) {
  if (auto lowest = type_map_ordering.lowest_common_type(a, b))
    return lowest;

  // Otherwise two instances of one generic, each open where the other
  // isn't, join: each hole takes the other's argument.
  if (!a.is_open() && !b.is_open())
    return std::nullopt;
  if (a.type_kind != b.type_kind ||
      (a.type_kind != TypeKind::Struct && a.type_kind != TypeKind::Enum))
    return std::nullopt;
  auto *a_key = a.instance_of();
  auto *b_key = b.instance_of();
  if (!a_key || !b_key || a_key->name != b_key->name ||
      a_key->type_args.size() != b_key->type_args.size())
    return std::nullopt;

  std::vector<Type> args;
  for (size_t i = 0; i < a_key->type_args.size(); i++) {
    auto &a_arg = a_key->type_args[i];
    auto &b_arg = b_key->type_args[i];
    if (a_arg.type_kind == TypeKind::Hole)
      args.push_back(b_arg);
    else if (b_arg.type_kind == TypeKind::Hole)
      args.push_back(a_arg);
    else if (auto common = common_type(a_arg, b_arg))
      args.push_back(*common);
    else
      return std::nullopt;
  }
  auto *enum_def = monomorphizer.generic_enums.find(a_key->name);
  auto *struct_def = monomorphizer.generic_structs.find(a_key->name);
  if (!enum_def && !struct_def)
    return std::nullopt;
  return generic_type_instance(enum_def ? enum_def->enum_name
                                        : struct_def->struct_name,
                               std::move(args));
}

void BiTypeCheckerVisitor::report_open_type(const ExprAST *expr) {
  std::optional<std::string> variant;
  if (auto *var = llvm::dyn_cast<VariableExprAST>(expr);
      var && props_.variable(var->id()).is_enum_unit_variant)
    variant = var->variableName.get_name();
  else if (auto *call = llvm::dyn_cast<CallExprAST>(expr);
           call && props_.call(call->id()).is_enum_constructor)
    variant = call->functionName.get_name();
  add_error(expr->get_location(),
            open_type_message(expr->get_type(), variant));
}

void BiTypeCheckerVisitor::instantiate_generic_callee(CallExprAST *ast) {
  auto &cp = props_.call(ast->id());
  if (cp.is_enum_constructor || cp.is_typeclass_call ||
      !cp.resolved_name.has_value())
    return;

  auto *generic_def =
      monomorphizer.generic_funcs.find(ast->functionName.mangled());
  if (!generic_def)
    return;

  auto key = MonomorphizedKey::from_bindings(
      ast->functionName.mangled(), generic_def->Prototype->type_params,
      cp.type_bindings);
  if (auto *cloned = monomorphizer.try_instantiate_func(generic_def, key,
                                                        cp.type_bindings)) {
    TopLevelScopeGuard scope(*this);
    cloned->accept_vis(this);
  }
}

// Expressions: synthesize visits its own children, so visit just forwards to it.
#define VISIT_VIA_SYNTHESIZE(Node)                                             \
  void BiTypeCheckerVisitor::visit(Node *ast) { ast->accept_synthesis(this); }

VISIT_VIA_SYNTHESIZE(CallExprAST)
VISIT_VIA_SYNTHESIZE(BinaryExprAST)
VISIT_VIA_SYNTHESIZE(BlockAST)
VISIT_VIA_SYNTHESIZE(IfExprAST)
VISIT_VIA_SYNTHESIZE(NumberExprAST)
VISIT_VIA_SYNTHESIZE(StringExprAST)
VISIT_VIA_SYNTHESIZE(BoolExprAST)
VISIT_VIA_SYNTHESIZE(CharExprAST)
VISIT_VIA_SYNTHESIZE(UnitExprAST)
VISIT_VIA_SYNTHESIZE(VariableExprAST)
VISIT_VIA_SYNTHESIZE(TypedVarAST)
VISIT_VIA_SYNTHESIZE(DerefExprAST)
VISIT_VIA_SYNTHESIZE(AddrOfExprAST)
VISIT_VIA_SYNTHESIZE(AllocExprAST)
VISIT_VIA_SYNTHESIZE(DropExprAST)
VISIT_VIA_SYNTHESIZE(CastExprAST)
VISIT_VIA_SYNTHESIZE(ArrayLiteralExprAST)
VISIT_VIA_SYNTHESIZE(RangeExprAST)
VISIT_VIA_SYNTHESIZE(IndexExprAST)
VISIT_VIA_SYNTHESIZE(LenExprAST)
VISIT_VIA_SYNTHESIZE(DimExprAST)
VISIT_VIA_SYNTHESIZE(UnaryNegExprAST)
VISIT_VIA_SYNTHESIZE(StructLiteralExprAST)
VISIT_VIA_SYNTHESIZE(FieldAccessExprAST)
VISIT_VIA_SYNTHESIZE(CaseExprAST)
VISIT_VIA_SYNTHESIZE(WhileExprAST)
VISIT_VIA_SYNTHESIZE(ForExprAST)
VISIT_VIA_SYNTHESIZE(TupleLiteralExprAST)
#undef VISIT_VIA_SYNTHESIZE

void BiTypeCheckerVisitor::visit(ReturnStmtAST *ast) {
  if (ast->return_expr)
    ast->return_expr->accept_vis(this);
  ast->accept_synthesis(this);
  auto t = ast->return_expr->accept_synthesis(this);
  check_returned(ast->return_expr.get(), t, ast->get_location());
}

void BiTypeCheckerVisitor::check_returned(ExprAST *value, const Type &t,
                                          sammine_util::Location loc) {
  auto scope_fn = this->ctx().enclosing_function;
  auto fn_type = std::get<FunctionType>(scope_fn->get_type().type_data);
  auto return_type = fn_type.get_return_type();
  // A poisoned value or return type has already had its error reported.
  if (t.is_poisoned() || return_type.is_poisoned())
    return;
  if (!type_map_ordering.compatible_to_from(return_type, t)) {
    report_type_mismatch(loc,
                         fmt::format("Wrong return type for function {}, "
                                     "expected {} but got {}",
                                     scope_fn->getFunctionName(),
                                     return_type.to_string(), t.to_string()),
                         return_type, t);
  } else if (value) {
    resolve_literal_type(value, return_type);
  }
}

void BiTypeCheckerVisitor::register_typeclass_decl(TypeClassDeclAST *ast) {
  if (type_class_defs.contains(ast->class_name))
    return;

  {
    ScopeGuard scope(*this);
    for (auto &param : ast->type_params)
      typename_to_type.registerNameT(param, Type::TypeParam(param));

    for (auto &proto : ast->methods) {
      proto->accept_synthesis(this);
      method_to_class[proto->functionName.mangled()] = ast->class_name;
    }
  }

  type_class_defs[ast->class_name] = ast;
}

std::optional<std::vector<Type>>
BiTypeCheckerVisitor::resolve_instance_types(TypeClassInstanceAST *ast) {
  // A generic instance's parameters are only visible in its own types.
  ScopeGuard scope(*this);
  for (auto &tp : ast->type_params)
    typename_to_type.registerNameT(tp, Type::TypeParam(tp));
  std::vector<Type> types;
  for (auto &type_expr : ast->concrete_type_exprs) {
    auto resolved = resolve_type_expr(type_expr.get());
    if (resolved.is_poisoned())
      return std::nullopt;
    types.push_back(resolved);
  }
  return types;
}

// Two instances overlap when some key matches both, so which one a call runs
// would be a choice. Like Rust, that is an error rather than a rule.
bool BiTypeCheckerVisitor::overlaps_registered_instance(
    TypeClassInstanceAST *ast, const std::vector<Type> &types) {
  // Rename this instance's parameters apart from the other one's: T and T
  // are unrelated in instance<T> A<T> and instance<T> B<T>.
  TypeBindings apart;
  for (auto &tp : ast->type_params)
    apart[tp] = Type::TypeParam(tp + "'");
  std::vector<Type> renamed;
  for (auto &type : types)
    renamed.push_back(substitute(type, apart));

  auto report = [&](const std::string &other) {
    this->add_error(
        ast->get_location(),
        fmt::format("This instance overlaps {}: both would apply to some "
                    "types, so which one runs would be ambiguous",
                    other));
    return true;
  };
  if (auto it = generic_instances.find(ast->class_name);
      it != generic_instances.end())
    for (auto &other : it->second)
      if (unifiable(renamed, other.types))
        return report(fmt::format(
            "instance<{}> {}<{}>", fmt::join(other.ast->type_params, ", "),
            ast->class_name,
            MonomorphizedKey{ast->class_name, other.types}.type_args_string()));
  if (!ast->is_generic())
    return false;
  for (auto &key : type_class_instances)
    if (key.name == ast->class_name && unifiable(renamed, key.type_args))
      return report(
          fmt::format("instance {}<{}>", key.name, key.type_args_string()));
  return false;
}

void BiTypeCheckerVisitor::register_typeclass_instance(
    TypeClassInstanceAST *ast) {
  auto types = resolve_instance_types(ast);
  if (!types)
    return;
  auto it = type_class_defs.find(ast->class_name);
  if (it == type_class_defs.end()) {
    this->add_error(ast->get_location(),
                    fmt::format("Unknown type class '{}'", ast->class_name));
    return;
  }

  auto *tc = it->second;
  if (types->size() != tc->type_params.size()) {
    this->add_error(
        ast->get_location(),
        fmt::format("Type class '{}' expects {} type argument(s), got {}",
                    ast->class_name, tc->type_params.size(), types->size()));
    return;
  }

  // Every parameter must be decided by the types, or a key wouldn't say
  // what the instance's copy is for.
  for (auto &tp : ast->type_params)
    if (std::ranges::none_of(*types, [&](const Type &type) {
          return contains_type_param(type, tp);
        })) {
      this->add_error(
          ast->get_location(),
          fmt::format("Type parameter '{}' of the instance is not used in "
                      "{}<...>",
                      tp, ast->class_name));
      return;
    }

  MonomorphizedKey tc_key{ast->class_name, *types};

  if (!ast->is_generic() && is_builtin_instance(tc_key)) {
    this->add_error(ast->get_location(),
                    fmt::format("{}<{}> is built in and can't be redefined",
                                ast->class_name, tc_key.type_args_string()));
    return;
  }
  if (!ast->is_generic() && type_class_instances.contains(tc_key)) {
    this->add_error(ast->get_location(),
                    fmt::format("Duplicate instance of {}<{}>", ast->class_name,
                                tc_key.type_args_string()));
    return;
  }
  if (overlaps_registered_instance(ast, *types))
    return;

  if (ast->is_generic()) {
    generic_instances[ast->class_name].push_back({ast, *types});
    return;
  }
  add_instance(ast, tc_key);
}

void BiTypeCheckerVisitor::add_instance(TypeClassInstanceAST *ast,
                                        const MonomorphizedKey &key) {
  for (auto &method : ast->methods) {
    std::string original_name = method->Prototype->functionName.get_name();
    method->Prototype->functionName = key.to_typeclass_name(original_name);
  }

  type_class_instances.insert(key);
  if (!ast->is_exported && ast->is_declaration)
    private_instances[key] = ast->defining_module;
}

bool BiTypeCheckerVisitor::instantiate_generic_instance(
    const MonomorphizedKey &key) {
  auto it = generic_instances.find(key.name);
  if (it == generic_instances.end())
    return false;
  // A copy is only made for types known in full: an untyped literal's or
  // None's are decided by their context first.
  if (std::ranges::any_of(key.type_args, [](const Type &type) {
        return type.is_polymorphic() || type.is_poisoned();
      }))
    return false;
  // Registration rejected overlaps, so at most one generic instance matches.
  for (auto &generic : it->second) {
    TypeBindings bindings;
    bool matches = generic.types.size() == key.type_args.size();
    for (size_t i = 0; matches && i < generic.types.size(); i++)
      matches = unify(generic.types[i], key.type_args[i], bindings);
    if (!matches)
      continue;

    auto *instance = monomorphizer.instantiate_instance(generic.ast, bindings);
    add_instance(instance, key);
    // The copy is compiled here, but a private generic instance stays its
    // module's: only that module's code may use it.
    if (!generic.ast->is_exported && generic.ast->is_declaration)
      private_instances[key] = generic.ast->defining_module;

    TopLevelScopeGuard scope(*this);
    for (auto &method : instance->methods)
      pre_register_function(method->Prototype.get());
    for (auto &method : instance->methods)
      method->accept_vis(this);
    return true;
  }
  return false;
}

// An instance is chosen by type, not by a name NameResolver could check, so
// its privacy is checked here: a private one is only for its module's code,
// such as that module's generic functions compiled here.
bool BiTypeCheckerVisitor::has_instance(const MonomorphizedKey &key) {
  if (is_builtin_instance(key))
    return true;
  if (!type_class_instances.contains(key) &&
      !instantiate_generic_instance(key))
    return false;
  auto it = private_instances.find(key);
  auto *fn = ctx().enclosing_function;
  return it == private_instances.end() ||
         (fn && fn->defining_module == it->second);
}

std::string
BiTypeCheckerVisitor::missing_instance(const MonomorphizedKey &key,
                                       std::string otherwise) const {
  auto it = private_instances.find(key);
  if (it == private_instances.end())
    return otherwise;
  return fmt::format("{}<{}> is private to module '{}'", key.name,
                     key.type_args_string(), it->second);
}

std::string
BiTypeCheckerVisitor::instance_template(const MonomorphizedKey &key) const {
  std::vector<std::string> methods;
  if (auto it = type_class_defs.find(key.name); it != type_class_defs.end()) {
    auto *tc = it->second;
    TypeBindings bindings;
    for (size_t i = 0; i < tc->type_params.size() && i < key.type_args.size();
         i++)
      bindings[tc->type_params[i]] = key.type_args[i];
    for (auto &proto : tc->methods) {
      auto fn = std::get<FunctionType>(
          substitute(proto->get_type(), bindings).type_data);
      std::vector<std::string> params;
      for (size_t i = 0; i < proto->parameterVectors.size(); i++)
        params.push_back(fmt::format("{}: {}", proto->parameterVectors[i]->name,
                                     fn.get_params_types()[i].to_string()));
      methods.push_back(fmt::format("let {}({}) -> {} {{ ... }}",
                                    proto->functionName.mangled(),
                                    fmt::join(params, ", "),
                                    fn.get_return_type().to_string()));
    }
  }
  auto body = methods.empty() ? std::string("...")
                              : fmt::format("{}", fmt::join(methods, " "));
  return fmt::format("instance {}<{}> {{ {} }}", key.name,
                     key.type_args_string(), body);
}

void BiTypeCheckerVisitor::visit(TypeClassDeclAST *ast) {
  // Already registered in first pass — nothing to do
  register_typeclass_decl(ast);
}

void BiTypeCheckerVisitor::visit(TypeClassInstanceAST *ast) {
  // Registration already done in first pass — just type-check method bodies.
  // A generic instance's are checked in each copy, as a generic function's.
  if (ast->is_generic())
    return;
  for (auto &method : ast->methods)
    method->accept_vis(this);
}

void BiTypeCheckerVisitor::visit(KernelDefAST *ast) {
  // Kernel defs: CPU visitors see the prototype but don't recurse into kernel
  // body
  ScopeGuard scope(*this);
  ctx().enclosing_kernel = ast;
  ast->Prototype->accept_vis(this);
  ast->accept_synthesis(this);
}

} // namespace sammine_lang::AST
