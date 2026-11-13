#include "typecheck/UniSubst.h"
#include "typecheck/Types.h"
namespace sammine_lang::AST {

bool contains_type_param(const Type &type, const std::string &param_name) {
  if (type.type_kind == TypeKind::TypeParam)
    return std::get<std::string>(type.type_data) == param_name;

  bool found = false;
  type.forEachInnerType([&](const Type &inner) {
    if (!found && contains_type_param(inner, param_name))
      found = true;
  });
  return found;
}

static Type substitute_parts(const Type &type, const TypeBindings &bindings);

// What an instance is an instance of, with `bindings` substituted into its
// type arguments: Box applied to (T) becomes Box applied to (i64).
static InstanceOf substitute_instance_of(const MonomorphizedKey *instance_of,
                                         const TypeBindings &bindings) {
  if (!instance_of)
    return nullptr;
  std::vector<Type> args;
  for (auto &arg : instance_of->type_args)
    args.push_back(substitute(arg, bindings));
  return std::make_shared<const MonomorphizedKey>(
      MonomorphizedKey{instance_of->name, std::move(args)});
}

// A type built from substituted parts keeps its own linearity: 'ptr<T> stays
// owning. A type parameter becomes what it is bound to, linearity and all.
Type substitute(const Type &type, const TypeBindings &bindings) {
  auto result = substitute_parts(type, bindings);
  if (type.type_kind != TypeKind::TypeParam)
    result.linearity = type.linearity;
  return result;
}

static Type substitute_parts(const Type &type, const TypeBindings &bindings) {
  switch (type.type_kind) {
  case TypeKind::TypeParam: {
    auto name = std::get<std::string>(type.type_data);
    auto it = bindings.find(name);
    if (it != bindings.end())
      return it->second;
    return type;
  }
  case TypeKind::Pointer: {
    auto &ptr = std::get<PointerType>(type.type_data);
    return Type{
        TypeKind::Pointer,
        PointerType(substitute(ptr.get_pointee(), bindings), ptr.is_buffer())};
  }
  case TypeKind::Array: {
    auto &arr = std::get<ArrayType>(type.type_data);
    return Type::Array(substitute(arr.get_element(), bindings), arr.get_size());
  }
  case TypeKind::Function: {
    auto &fn = std::get<FunctionType>(type.type_data);
    std::vector<Type> total;
    for (auto &p : fn.get_params_types())
      total.push_back(substitute(p, bindings));
    total.push_back(substitute(fn.get_return_type(), bindings));
    return Type::Function(std::move(total));
  }
  case TypeKind::Tuple: {
    auto &tt = std::get<TupleType>(type.type_data);
    std::vector<Type> elems;
    for (auto &e : tt.get_element_types())
      elems.push_back(substitute(e, bindings));
    return Type::Tuple(std::move(elems));
  }
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(type.type_data);
    std::vector<Type> ftypes;
    bool changed = false;
    for (auto &ft : st.get_field_types()) {
      auto sub = substitute(ft, bindings);
      if (sub != ft)
        changed = true;
      ftypes.push_back(sub);
    }
    if (!changed)
      return type;
    return Type::Struct(
        st.get_name(), st.get_field_names(), std::move(ftypes),
        substitute_instance_of(st.get_instance_of(), bindings));
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(type.type_data);
    std::vector<EnumType::VariantInfo> new_variants;
    bool changed = false;
    for (auto &v : et.get_variants()) {
      std::vector<Type> new_payloads;
      for (auto &pt : v.payload_types) {
        auto sub = substitute(pt, bindings);
        if (sub != pt)
          changed = true;
        new_payloads.push_back(sub);
      }
      new_variants.push_back(
          {v.name, std::move(new_payloads), v.discriminant_value});
    }
    if (!changed)
      return type;
    return Type::Enum(et.get_name(), std::move(new_variants),
                      et.is_integer_backed(), et.get_backing_type(),
                      substitute_instance_of(et.get_instance_of(), bindings));
  }
  // Scalar and pseudo-types: nothing to substitute
  case TypeKind::I32_t:
  case TypeKind::I64_t:
  case TypeKind::U32_t:
  case TypeKind::U64_t:
  case TypeKind::F64_t:
  case TypeKind::F32_t:
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::Char:
  case TypeKind::String:
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::Integer:
  case TypeKind::Flt:
  case TypeKind::Hole:
  case TypeKind::Generic:
    return type;
  default:
    return type;
  }
}

// Two instances of one generic type unify through their type arguments, as
// Box<T> with Box<i64> binds T to i64. nullopt when they aren't instances of
// one generic type.
static bool unify_impl(const Type &pattern, const Type &concrete,
                       TypeBindings &bindings, bool both_sides);

static std::optional<bool> unify_instances(const MonomorphizedKey *pattern,
                                           const MonomorphizedKey *concrete,
                                           TypeBindings &bindings,
                                           bool both_sides) {
  if (!pattern || !concrete || pattern->name != concrete->name ||
      pattern->type_args.size() != concrete->type_args.size())
    return std::nullopt;
  for (size_t i = 0; i < pattern->type_args.size(); i++)
    if (!unify_impl(pattern->type_args[i], concrete->type_args[i], bindings,
                    both_sides))
      return false;
  return true;
}

// Follows `type` through the bindings while it is a bound type parameter.
static Type walk(Type type, const TypeBindings &bindings) {
  while (type.type_kind == TypeKind::TypeParam) {
    auto it = bindings.find(std::get<std::string>(type.type_data));
    if (it == bindings.end())
      break;
    type = it->second;
  }
  return type;
}

// Whether `param` occurs in `type` once every bound parameter in it is
// replaced by what it is bound to.
static bool occurs(const std::string &param, const Type &type,
                   const TypeBindings &bindings) {
  auto t = walk(type, bindings);
  if (t.type_kind == TypeKind::TypeParam)
    return std::get<std::string>(t.type_data) == param;
  bool found = false;
  t.forEachInnerType([&](const Type &inner) {
    if (!found && occurs(param, inner, bindings))
      found = true;
  });
  return found;
}

// When either side is a type parameter, binds it to the other side.
// nullopt when neither is one.
static std::optional<bool> unify_params(const Type &a, const Type &b,
                                        TypeBindings &bindings) {
  auto bind = [&](const Type &param, const Type &to) {
    auto name = std::get<std::string>(param.type_data);
    if (occurs(name, to, bindings))
      return false;
    bindings[name] = to;
    return true;
  };
  bool a_param = a.type_kind == TypeKind::TypeParam;
  bool b_param = b.type_kind == TypeKind::TypeParam;
  if (a_param && b_param &&
      std::get<std::string>(a.type_data) == std::get<std::string>(b.type_data))
    return true;
  if (a_param)
    return bind(a, b);
  if (b_param)
    return bind(b, a);
  return std::nullopt;
}

bool unify(const Type &pattern, const Type &concrete, TypeBindings &bindings) {
  return unify_impl(pattern, concrete, bindings, /*both_sides=*/false);
}

bool unifiable(std::span<const Type> a, std::span<const Type> b) {
  if (a.size() != b.size())
    return false;
  TypeBindings bindings;
  for (size_t i = 0; i < a.size(); i++)
    if (!unify_impl(a[i], b[i], bindings, /*both_sides=*/true))
      return false;
  return true;
}

// One-sided, only `pattern` holds parameters to bind. Both-sided, either
// may, and a parameter is followed through what it is already bound to.
static bool unify_impl(const Type &pattern_in, const Type &concrete_in,
                       TypeBindings &bindings, bool both_sides) {
  auto pattern = both_sides ? walk(pattern_in, bindings) : pattern_in;
  auto concrete = both_sides ? walk(concrete_in, bindings) : concrete_in;
  if (both_sides)
    if (auto unified = unify_params(pattern, concrete, bindings))
      return *unified;
  // An open type argument says nothing, so it fits any pattern and binds
  // nothing: or_else(None, 5i64) learns T from 5i64.
  if (concrete.type_kind == TypeKind::Hole)
    return true;
  if (pattern.type_kind == TypeKind::TypeParam) {
    auto name = std::get<std::string>(pattern.type_data);
    // Occurs check
    if (contains_type_param(concrete, name))
      return false;
    auto it = bindings.find(name);
    if (it != bindings.end()) {
      return it->second == concrete;
    }
    bindings[name] = concrete;
    return true;
  }

  if (pattern.type_kind != concrete.type_kind)
    return false;

  switch (pattern.type_kind) {
  case TypeKind::Pointer: {
    auto &pp = std::get<PointerType>(pattern.type_data);
    auto &cp = std::get<PointerType>(concrete.type_data);
    // A box isn't a buffer; a view, ptr<T>, may be lent either.
    if (pattern.is_linear_v() && pp.is_buffer() != cp.is_buffer())
      return false;
    return unify_impl(pp.get_pointee(), cp.get_pointee(), bindings, both_sides);
  }
  case TypeKind::Array: {
    auto &pa = std::get<ArrayType>(pattern.type_data);
    auto &ca = std::get<ArrayType>(concrete.type_data);
    if (pa.get_size() != ca.get_size())
      return false;
    return unify_impl(pa.get_element(), ca.get_element(), bindings, both_sides);
  }
  case TypeKind::Function: {
    auto &pf = std::get<FunctionType>(pattern.type_data);
    auto &cf = std::get<FunctionType>(concrete.type_data);
    auto pp = pf.get_params_types();
    auto cp = cf.get_params_types();
    if (pp.size() != cp.size())
      return false;
    for (size_t i = 0; i < pp.size(); i++)
      if (!unify_impl(pp[i], cp[i], bindings, both_sides))
        return false;
    return unify_impl(pf.get_return_type(), cf.get_return_type(), bindings, both_sides);
  }
  case TypeKind::Tuple: {
    auto &pt = std::get<TupleType>(pattern.type_data);
    auto &ct = std::get<TupleType>(concrete.type_data);
    if (pt.size() != ct.size())
      return false;
    for (size_t i = 0; i < pt.size(); i++)
      if (!unify_impl(pt.get_element(i), ct.get_element(i), bindings, both_sides))
        return false;
    return true;
  }
  case TypeKind::Struct: {
    auto &ps = std::get<StructType>(pattern.type_data);
    auto &cs = std::get<StructType>(concrete.type_data);
    if (auto unified = unify_instances(ps.get_instance_of(),
                                       cs.get_instance_of(), bindings,
                                       both_sides))
      return *unified;
    if (ps.get_name().mangled() != cs.get_name().mangled())
      return false;
    auto &pft = ps.get_field_types();
    auto &cft = cs.get_field_types();
    if (pft.size() != cft.size())
      return false;
    for (size_t i = 0; i < pft.size(); i++)
      if (!unify_impl(pft[i], cft[i], bindings, both_sides))
        return false;
    return true;
  }
  case TypeKind::Enum: {
    auto &pe = std::get<EnumType>(pattern.type_data);
    auto &ce = std::get<EnumType>(concrete.type_data);
    if (auto unified = unify_instances(pe.get_instance_of(),
                                       ce.get_instance_of(), bindings,
                                       both_sides))
      return *unified;
    if (pe.get_name().mangled() != ce.get_name().mangled())
      return false;
    auto &pvs = pe.get_variants();
    auto &cvs = ce.get_variants();
    if (pvs.size() != cvs.size())
      return false;
    for (size_t i = 0; i < pvs.size(); i++) {
      if (pvs[i].name != cvs[i].name)
        return false;
      if (pvs[i].payload_types.size() != cvs[i].payload_types.size())
        return false;
      for (size_t j = 0; j < pvs[i].payload_types.size(); j++)
        if (!unify_impl(pvs[i].payload_types[j], cvs[i].payload_types[j], bindings, both_sides))
          return false;
    }
    return true;
  }
  // Scalar and pseudo-types: type_kind equality already checked above
  case TypeKind::I32_t:
  case TypeKind::I64_t:
  case TypeKind::U32_t:
  case TypeKind::U64_t:
  case TypeKind::F64_t:
  case TypeKind::F32_t:
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::Char:
  case TypeKind::String:
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::Integer:
  case TypeKind::Flt:
  case TypeKind::TypeParam:
  case TypeKind::Generic:
    return true;
  default:
    return false;
  }
}

} // namespace sammine_lang::AST
