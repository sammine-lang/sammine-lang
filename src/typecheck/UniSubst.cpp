#include "typecheck/UniSubst.h"
#include "typecheck/Types.h"
namespace sammine_lang::AST {


bool contains_type_param(const Type &type,
                                               const std::string &param_name) {
  if (type.type_kind == TypeKind::TypeParam)
    return std::get<std::string>(type.type_data) == param_name;

  bool found = false;
  type.forEachInnerType([&](const Type &inner) {
    if (!found && contains_type_param(inner, param_name))
      found = true;
  });
  return found;
}

Type substitute(
    const Type &type, const TypeBindings &bindings) {
  switch (type.type_kind) {
  case TypeKind::TypeParam: {
    auto name = std::get<std::string>(type.type_data);
    auto it = bindings.find(name);
    if (it != bindings.end())
      return it->second;
    return type;
  }
  case TypeKind::Pointer: {
    auto pointee = std::get<PointerType>(type.type_data).get_pointee();
    return Type::Pointer(substitute(pointee, bindings));
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
      if (sub != ft) changed = true;
      ftypes.push_back(sub);
    }
    if (!changed) return type;
    return Type::Struct(st.get_name(),
                        st.get_field_names(), std::move(ftypes));
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(type.type_data);
    std::vector<EnumType::VariantInfo> new_variants;
    bool changed = false;
    for (auto &v : et.get_variants()) {
      std::vector<Type> new_payloads;
      for (auto &pt : v.payload_types) {
        auto sub = substitute(pt, bindings);
        if (sub != pt) changed = true;
        new_payloads.push_back(sub);
      }
      new_variants.push_back({v.name, std::move(new_payloads), v.discriminant_value});
    }
    if (!changed) return type;
    return Type::Enum(et.get_name(), std::move(new_variants),
                      et.is_integer_backed(), et.get_backing_type());
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
  case TypeKind::Generic:
    return type;
  }
}




bool unify(
    const Type &pattern, const Type &concrete,
    TypeBindings &bindings) {
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
    auto pp = std::get<PointerType>(pattern.type_data).get_pointee();
    auto cp = std::get<PointerType>(concrete.type_data).get_pointee();
    return unify(pp, cp, bindings);
  }
  case TypeKind::Array: {
    auto &pa = std::get<ArrayType>(pattern.type_data);
    auto &ca = std::get<ArrayType>(concrete.type_data);
    if (pa.get_size() != ca.get_size())
      return false;
    return unify(pa.get_element(), ca.get_element(), bindings);
  }
  case TypeKind::Function: {
    auto &pf = std::get<FunctionType>(pattern.type_data);
    auto &cf = std::get<FunctionType>(concrete.type_data);
    auto pp = pf.get_params_types();
    auto cp = cf.get_params_types();
    if (pp.size() != cp.size())
      return false;
    for (size_t i = 0; i < pp.size(); i++)
      if (!unify(pp[i], cp[i], bindings))
        return false;
    return unify(pf.get_return_type(), cf.get_return_type(), bindings);
  }
  case TypeKind::Tuple: {
    auto &pt = std::get<TupleType>(pattern.type_data);
    auto &ct = std::get<TupleType>(concrete.type_data);
    if (pt.size() != ct.size())
      return false;
    for (size_t i = 0; i < pt.size(); i++)
      if (!unify(pt.get_element(i), ct.get_element(i), bindings))
        return false;
    return true;
  }
  case TypeKind::Struct: {
    auto &ps = std::get<StructType>(pattern.type_data);
    auto &cs = std::get<StructType>(concrete.type_data);
    if (ps.get_name().mangled() != cs.get_name().mangled())
      return false;
    auto &pft = ps.get_field_types();
    auto &cft = cs.get_field_types();
    if (pft.size() != cft.size())
      return false;
    for (size_t i = 0; i < pft.size(); i++)
      if (!unify(pft[i], cft[i], bindings))
        return false;
    return true;
  }
  case TypeKind::Enum: {
    auto &pe = std::get<EnumType>(pattern.type_data);
    auto &ce = std::get<EnumType>(concrete.type_data);
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
        if (!unify(pvs[i].payload_types[j], cvs[i].payload_types[j], bindings))
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
  }
}

}
