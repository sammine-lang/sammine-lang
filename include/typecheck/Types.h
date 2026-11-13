#pragma once
#include "util/QualifiedName.h"
#include "util/Utilities.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>
//! \file Types.h
//! \brief Defines the core Type system for Sammine
/// Discriminator for all types in the sammine type system.
/// Concrete types (I32_t..Tuple) represent real runtime values.
/// Pseudo-types (Never..Generic) exist only during type checking.
enum class TypeKind {
  // --- Concrete scalar types ---
  I32_t,
  I64_t,
  U32_t, // unsigned 32-bit — requires explicit suffix (42u32)
  U64_t, // unsigned 64-bit — requires explicit suffix (100u64)
  F64_t,
  F32_t, // 32-bit float — suffix syntax: 3.14f32
  Unit,  // void-like type, written as ()
  Bool,
  Char,
  String,
  // --- Compound types (carry TypeData payload) ---
  Function,
  Pointer,
  Array,
  Struct,
  Enum,
  Tuple,
  // --- Pseudo-types (type checker internal, never reach codegen) ---
  Never,       // diverging expressions (e.g. return, abort)
  NonExistent, // not-yet-typed AST node (default before synthesis)
  Poisoned,    // type error sentinel — suppresses cascading errors
  Integer,     // polymorphic integer literal — flows into i32/i64/u32/u64 via
               // context
  Flt,         // polymorphic float literal — flows into f64/f32 via context
  Hole,        // a type argument the context decides, as T in None: Option<_>
  TypeParam,   // generic type parameter (e.g. T in identity<T>)
  Generic      // uninstantiated generic function/struct template
};

struct Type;
struct MonomorphizedKey;
/// The generic type a struct or enum instantiates and the type arguments it
/// is given: Box<i64> is Box applied to (i64), and Box<T> in a generic
/// signature is Box applied to (T). Null for a type that is no instance.
using InstanceOf = std::shared_ptr<const MonomorphizedKey>;
class FunctionType;

using TypePtr = std::shared_ptr<Type>;
/// Function type: (param_types) -> return_type, with optional vararg for C FFI.
class FunctionType {
  std::vector<Type> param_types;
  TypePtr return_type; // shared_ptr because Type is incomplete here
  bool var_arg = false;

public:
  bool operator==(const FunctionType &t) const;

  std::span<const Type> get_params_types() const;
  Type get_return_type() const;
  bool is_var_arg() const { return var_arg; }

  FunctionType(std::vector<Type> param_types, Type return_type,
               bool var_arg = false);
  // Legacy: total_types = [params..., return_type]
  FunctionType(const std::vector<Type> &total_types, bool var_arg = false);
};
/// Pointer type: ptr<T> (a view), 'ptr<T> (owns one T, a box) or 'buf<T>
/// (owns room for many T, a buffer: its owner says how many are there).
class PointerType {
  TypePtr pointee;
  bool buffer = false;

public:
  bool operator==(const PointerType &t) const;
  Type get_pointee() const;
  bool is_buffer() const { return buffer; }
  explicit PointerType(Type pointee, bool buffer = false);
};
/// Fixed-size array type: [N]T. Bounds-checked at runtime.
class ArrayType {
  TypePtr element;
  size_t size;

public:
  bool operator==(const ArrayType &t) const;
  Type get_element() const;
  size_t get_size() const;
  ArrayType(Type element, size_t size);
};
/// Named product type with ordered fields. Name is qualified (e.g. mod::Point).
/// Equality is by name. The fields are shared by every copy of the type, so a
/// type can be declared first and its fields filled in once with `define`,
/// letting types refer to types declared later.
class StructType {
  struct Body {
    std::vector<std::string> field_names;
    std::vector<Type> field_types;
    bool defined = false;
  };
  sammine_util::QualifiedName name;
  std::shared_ptr<Body> body;
  InstanceOf instance_of;

public:
  bool operator==(const StructType &t) const;
  const sammine_util::QualifiedName &get_name() const { return name; }
  const MonomorphizedKey *get_instance_of() const { return instance_of.get(); }
  const std::vector<std::string> &get_field_names() const {
    return body->field_names;
  }
  const std::vector<Type> &get_field_types() const {
    return body->field_types;
  }
  std::optional<size_t> get_field_index(const std::string &field) const;
  Type get_field_type(size_t idx) const;
  size_t field_count() const { return body->field_names.size(); }
  bool is_defined() const { return body->defined; }
  /// Fill in the fields of a declared type, for every copy of it.
  void define(std::vector<std::string> field_names,
              std::vector<Type> field_types) const;
  /// A declared type, with its fields not filled in yet.
  explicit StructType(sammine_util::QualifiedName name,
                      InstanceOf instance_of = nullptr);
  StructType(sammine_util::QualifiedName name,
             std::vector<std::string> field_names,
             std::vector<Type> field_types, InstanceOf instance_of = nullptr);
};
/// Sum type with named variants. Supports unit variants, payload variants,
/// and integer-backed enums (explicit discriminant values, optional backing
/// type).
class EnumType {
  sammine_util::QualifiedName name;

public:
  struct VariantInfo {
    std::string name;
    std::vector<Type> payload_types;           // empty for unit variants
    std::optional<int64_t> discriminant_value; // set for integer-backed enums
  };

private:
  /// Shared by every copy of the type, as StructType's fields are.
  struct Body {
    std::vector<VariantInfo> variants;
    bool integer_backed = false;
    TypeKind backing_type = TypeKind::I32_t;
    bool defined = false;
  };
  std::shared_ptr<Body> body;
  InstanceOf instance_of;

public:
  bool operator==(const EnumType &t) const;
  const sammine_util::QualifiedName &get_name() const { return name; }
  const MonomorphizedKey *get_instance_of() const { return instance_of.get(); }
  const std::vector<VariantInfo> &get_variants() const {
    return body->variants;
  }
  std::optional<size_t>
  get_variant_index(const std::string &variant_name) const;
  const VariantInfo &get_variant(size_t idx) const {
    return body->variants[idx];
  }
  size_t variant_count() const { return body->variants.size(); }
  bool is_integer_backed() const { return body->integer_backed; }
  TypeKind get_backing_type() const { return body->backing_type; }
  bool is_defined() const { return body->defined; }
  /// Fill in the variants of a declared type, for every copy of it.
  void define(std::vector<VariantInfo> variants, bool integer_backed,
              TypeKind backing_type) const;
  /// A declared type, with its variants not filled in yet.
  explicit EnumType(sammine_util::QualifiedName name,
                    InstanceOf instance_of = nullptr);
  EnumType(sammine_util::QualifiedName name, std::vector<VariantInfo> variants,
           bool integer_backed = false,
           TypeKind backing_type = TypeKind::I32_t,
           InstanceOf instance_of = nullptr);
};
/// Anonymous product type: (T, U, V). Supports destructuring via let (a, b) =
/// t.
class TupleType {
  std::shared_ptr<std::vector<Type>> element_types;

public:
  bool operator==(const TupleType &t) const;
  const std::vector<Type> &get_element_types() const;
  size_t size() const;
  Type get_element(size_t idx) const;
  TupleType(std::vector<Type> element_types);
};
/// Payload for compound types. std::string holds TypeParam names and String
/// values. std::monostate for scalar types that carry no extra data.
using TypeData = std::variant<FunctionType, PointerType, ArrayType, StructType,
                              EnumType, TupleType, std::string, std::monostate>;

enum class Linearity : uint8_t {
  NonLinear = 0,
  Linear = 1
}; // Linear = must be consumed exactly once

/// Core type representation. Every AST node gets a Type via ASTProperties.
/// type_kind discriminates, type_data carries compound type details,
/// mutability/linearity are orthogonal qualifiers.
struct Type {
  TypeKind type_kind;
  TypeData type_data;
  Linearity linearity = Linearity::NonLinear;

  bool is_linear_v() const { return linearity == Linearity::Linear; }
  // Constructors
  Type() : type_kind(TypeKind::NonExistent), type_data(std::monostate()) {}
  static Type I32_t() { return Type{TypeKind::I32_t, std::monostate()}; }
  static Type I64_t() { return Type{TypeKind::I64_t, std::monostate()}; }
  static Type U32_t() { return Type{TypeKind::U32_t, std::monostate()}; }
  static Type U64_t() { return Type{TypeKind::U64_t, std::monostate()}; }
  static Type F64_t() { return Type{TypeKind::F64_t, std::monostate()}; }
  static Type F32_t() { return Type{TypeKind::F32_t, std::monostate()}; }
  static Type Bool() { return Type{TypeKind::Bool, std::monostate()}; }
  static Type Char() { return Type{TypeKind::Char, std::monostate()}; }
  static Type
  Poisoned(std::source_location src = std::source_location::current()) {
    auto path = std::filesystem::path(src.file_name()).filename().string();
    return Type{TypeKind::Poisoned, fmt::format("{}:{}", path, src.line())};
  }
  static Type Unit() { return Type{TypeKind::Unit, std::monostate()}; }
  static Type Never() { return Type{TypeKind::Never, std::monostate()}; }
  static Type Integer() { return Type{TypeKind::Integer, std::monostate()}; }
  static Type Flt() { return Type{TypeKind::Flt, std::monostate()}; }
  static Type Hole() { return Type{TypeKind::Hole, std::monostate()}; }
  static Type Generic() { return Type{TypeKind::Generic, std::monostate()}; }
  static Type String(const std::string &str) {
    return Type{TypeKind::String, str};
  }
  static Type NonExistent() {
    return Type{TypeKind::NonExistent, std::monostate()};
  }
  static Type TypeParam(const std::string &name) {
    return Type{TypeKind::TypeParam, name};
  }
  static Type Pointer(Type pointee) {
    return Type{TypeKind::Pointer, PointerType(pointee)};
  }
  /// 'buf<T>: an owned buffer of T.
  static Type Buffer(Type pointee) {
    Type result{TypeKind::Pointer, PointerType(pointee, /*buffer=*/true)};
    result.linearity = Linearity::Linear;
    return result;
  }
  /// Whether this is a 'buf<T>.
  bool is_buffer() const {
    return type_kind == TypeKind::Pointer &&
           std::get<PointerType>(type_data).is_buffer();
  }
  static Type Array(Type element, size_t size) {
    return Type{TypeKind::Array, ArrayType(element, size)};
  }
  static Type Struct(sammine_util::QualifiedName name,
                     std::vector<std::string> field_names,
                     std::vector<Type> field_types,
                     InstanceOf instance_of = nullptr) {
    return Type{TypeKind::Struct,
                StructType(std::move(name), std::move(field_names),
                           std::move(field_types), std::move(instance_of))};
  }
  static Type Enum(sammine_util::QualifiedName name,
                   std::vector<EnumType::VariantInfo> variants,
                   bool integer_backed = false,
                   TypeKind backing_type = TypeKind::I32_t,
                   InstanceOf instance_of = nullptr) {
    return Type{TypeKind::Enum,
                EnumType(std::move(name), std::move(variants), integer_backed,
                         backing_type, std::move(instance_of))};
  }
  /// A struct whose fields are filled in later with StructType::define.
  static Type DeclaredStruct(sammine_util::QualifiedName name,
                             InstanceOf instance_of = nullptr) {
    return Type{TypeKind::Struct,
                StructType(std::move(name), std::move(instance_of))};
  }
  /// An enum whose variants are filled in later with EnumType::define.
  static Type DeclaredEnum(sammine_util::QualifiedName name,
                           InstanceOf instance_of = nullptr) {
    return Type{TypeKind::Enum,
                EnumType(std::move(name), std::move(instance_of))};
  }
  static Type Tuple(std::vector<Type> element_types) {
    return Type{TypeKind::Tuple, TupleType(std::move(element_types))};
  }
  static Type Function(std::vector<Type> params, bool var_arg = false);
  explicit operator bool() const {
    return this->type_kind != TypeKind::Poisoned;
  }
  bool synthesized() const { return this->type_kind != TypeKind::NonExistent; }
  Type(TypeKind type_kind_, TypeData type_data_)
      : type_kind(type_kind_), type_data(type_data_) {}

  Type(const Type &other);
  Type(Type &&other) noexcept;
  Type &operator=(const Type &other);
  Type &operator=(Type &&other) noexcept;
  ~Type();

  bool operator==(const Type &other) const;

  bool operator!=(const Type &other) const;

  std::string to_string() const {
    switch (type_kind) {
    case TypeKind::I32_t:
      return "i32";
    case TypeKind::I64_t:
      return "i64";
    case TypeKind::U32_t:
      return "u32";
    case TypeKind::U64_t:
      return "u64";
    case TypeKind::F64_t:
      return "f64";
    case TypeKind::F32_t:
      return "f32";
    case TypeKind::Unit:
      return "()";
    case TypeKind::Struct:
      return std::get<StructType>(type_data).get_name().mangled();
    case TypeKind::Enum:
      return std::get<EnumType>(type_data).get_name().mangled();
    case TypeKind::Bool:
      return "bool";
    case TypeKind::Char:
      return "char";
    case TypeKind::Pointer: {
      auto &ptr = std::get<PointerType>(type_data);
      auto spelled = ptr.is_buffer() ? "buf<" : "ptr<";
      return (linearity == Linearity::Linear ? "'" : "") +
             std::string(spelled) + ptr.get_pointee().to_string() + ">";
    }
    case TypeKind::Array:
      return "[" + std::to_string(std::get<ArrayType>(type_data).get_size()) +
             "]" + std::get<ArrayType>(type_data).get_element().to_string();
    case TypeKind::Function: {
      std::string res = "(";
      auto fn_type = std::get<FunctionType>(type_data);
      auto param = fn_type.get_params_types();
      for (size_t i = 0; i < param.size(); i++) {
        res += param[i].to_string();
        if (i != param.size() - 1)
          res += ", ";
      }
      res += ") -> ";
      res += fn_type.get_return_type().to_string();

      return res;
    }
    case TypeKind::Tuple: {
      std::string res = "(";
      auto &tt = std::get<TupleType>(type_data);
      for (size_t i = 0; i < tt.size(); i++) {
        res += tt.get_element(i).to_string();
        if (i != tt.size() - 1)
          res += ", ";
      }
      res += ")";
      return res;
    }
    case TypeKind::Never:
      return "!";
    case TypeKind::NonExistent:
      return "??";
    case TypeKind::Poisoned:
      // Where it was poisoned, for whoever debugs the compiler.
      if (auto *origin = std::get_if<std::string>(&type_data))
        return fmt::format("{}({})", sammine_util::kPoisonedMarker, *origin);
      return std::string(sammine_util::kPoisonedMarker);
    case TypeKind::String:
      return fmt::format("\"{}\"", std::get<std::string>(type_data));
    case TypeKind::Integer:
      return "numeric literal";
    case TypeKind::Flt:
      return "float literal";
    case TypeKind::Hole:
      return "_";
    case TypeKind::TypeParam:
      return std::get<std::string>(type_data);
    case TypeKind::Generic:
      return "generic template";
    }
    sammine_util::abort("Reaching the end of switch case and still cant "
                        "convert to string, blame Jasmine (badumbatish)!!!!!");
  }

  bool is_poisoned() const { return this->type_kind == TypeKind::Poisoned; }

  /// Returns true for types that can appear as literal values (scalars +
  /// polymorphic).
  bool is_literal() const {
    switch (type_kind) {
    case TypeKind::I32_t:
    case TypeKind::I64_t:
    case TypeKind::U32_t:
    case TypeKind::U64_t:
    case TypeKind::F64_t:
    case TypeKind::F32_t:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Unit:
    case TypeKind::String:
    case TypeKind::Integer:
    case TypeKind::Flt:
      return true;
    case TypeKind::TypeParam:
      return false;
    default:
      return false;
    }
  }

  /// True for unresolved numeric literals (Integer/Flt) that need context to
  /// specialize.
  bool is_polymorphic_numeric() const {
    return type_kind == TypeKind::Integer || type_kind == TypeKind::Flt;
  }

  /// What a struct or enum instantiates, as Option applied to (i64): null
  /// for any other type, or one that instantiates no generic.
  const MonomorphizedKey *instance_of() const {
    if (type_kind == TypeKind::Struct)
      return std::get<StructType>(type_data).get_instance_of();
    if (type_kind == TypeKind::Enum)
      return std::get<EnumType>(type_data).get_instance_of();
    return nullptr;
  }

  /// Whether a type argument is still open, as in None's Option<_>, here or
  /// in any type inside: the context has yet to decide it.
  bool is_open() const;

  /// True for a type its context specializes: a numeric literal's, or one
  /// with an open type argument.
  bool is_polymorphic() const { return is_polymorphic_numeric() || is_open(); }

  bool is_integer() const {
    return type_kind == TypeKind::I32_t || type_kind == TypeKind::I64_t ||
           type_kind == TypeKind::U32_t || type_kind == TypeKind::U64_t ||
           type_kind == TypeKind::Integer;
  }

  bool is_unsigned() const {
    return type_kind == TypeKind::U32_t || type_kind == TypeKind::U64_t;
  }

  bool is_signed() const {
    return type_kind == TypeKind::I32_t || type_kind == TypeKind::I64_t;
  }

  bool is_float() const {
    return type_kind == TypeKind::F32_t || type_kind == TypeKind::F64_t ||
           type_kind == TypeKind::Flt;
  }

  bool is_never() const { return type_kind == TypeKind::Never; }

  bool is_numeric() const { return is_integer() || is_float(); }

  /// True for concrete scalar types valid as literal case-expression
  /// scrutinees. Excludes floats (equality is unreliable) and polymorphic types
  /// (must be resolved first).
  bool is_matchable_scalar() const {
    switch (type_kind) {
    case TypeKind::I32_t:
    case TypeKind::I64_t:
    case TypeKind::U32_t:
    case TypeKind::U64_t:
    case TypeKind::Bool:
    case TypeKind::Char:
      return true;
    default:
      return false;
    }
  }

  /// True for compound types that contain inner types (used by
  /// forEachInnerType).
  bool isTypeWrapping() const {
    switch (type_kind) {
    case TypeKind::Pointer:
    case TypeKind::Array:
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::Function:
    case TypeKind::Tuple:
      return true;
    default:
      return false;
    }
  }

  /// Recursively checks if this type or any nested type has linear ownership.
  bool containsLinearTypes() const {
    if (linearity == Linearity::Linear)
      return true;
    // A view owns nothing, whatever it points at, nor does a function value,
    // whatever it takes: a closure can't bind an owning value.
    if (type_kind == TypeKind::Pointer || type_kind == TypeKind::Function)
      return false;
    bool found = false;
    forEachInnerType([&](const Type &inner) {
      if (inner.containsLinearTypes())
        found = true;
    });
    return found;
  }

  /// Recursively checks if this type or any nested type contains a non-linear
  /// pointer.
  bool containsNonLinearPtr() const {
    if (type_kind == TypeKind::Pointer && linearity != Linearity::Linear)
      return true;
    bool found = false;
    forEachInnerType([&](const Type &inner) {
      if (inner.containsNonLinearPtr())
        found = true;
    });
    return found;
  }

  /// Recursively checks if this type or any nested type is a TypeParam.
  bool containsTypeParam() const {
    if (type_kind == TypeKind::TypeParam)
      return true;
    bool found = false;
    forEachInnerType([&](const Type &inner) {
      if (inner.containsTypeParam())
        found = true;
    });
    return found;
  }

  /// Visits all immediately-nested types (pointee, elements, fields, params,
  /// etc.).
  template <typename F> void forEachInnerType(F &&callback) const {
    switch (type_kind) {
    case TypeKind::Pointer: {
      auto &pt = std::get<PointerType>(type_data);
      callback(pt.get_pointee());
      break;
    }
    case TypeKind::Array: {
      auto &at = std::get<ArrayType>(type_data);
      callback(at.get_element());
      break;
    }
    case TypeKind::Struct: {
      auto &st = std::get<StructType>(type_data);
      for (auto &ft : st.get_field_types())
        callback(ft);
      break;
    }
    case TypeKind::Enum: {
      auto &et = std::get<EnumType>(type_data);
      for (auto &variant : et.get_variants())
        for (auto &pt : variant.payload_types)
          callback(pt);
      break;
    }
    case TypeKind::Function: {
      auto &fn = std::get<FunctionType>(type_data);
      for (auto &p : fn.get_params_types())
        callback(p);
      callback(fn.get_return_type());
      break;
    }
    case TypeKind::Tuple: {
      auto &tt = std::get<TupleType>(type_data);
      for (auto &et : tt.get_element_types())
        callback(et);
      break;
    }
    default:
      break;
    }
  }

  operator std::string() const { return to_string(); }
};

/// Utility for combining hash values (boost-style).
inline void hash_combine(size_t &seed, size_t h) {
  seed ^= h + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

/// Hashes TypeKind + TypeData only, consistent with operator==.
/// Linearity is intentionally excluded: it is a qualifier on pointers,
/// not part of type identity. Linear vs non-linear pointers generate
/// identical code structurally, so e.g. identity<ptr<i32>> and
/// identity<'ptr<i32>> are the same monomorphization.
namespace std {
template <> struct hash<Type> {
  size_t operator()(const Type &t) const;
};
} // namespace std

/// Structured key for maps that associate a name with concrete type arguments.
/// Used for typeclass instance lookup, monomorphization dedup, etc.
/// Provides methods to produce strings (for codegen symbols / error messages)
/// so that all type-to-string formatting goes through one place.
struct MonomorphizedKey {
  std::string name;
  std::vector<Type> type_args;
  bool operator==(const MonomorphizedKey &) const = default;

  /// Build from type param names + bindings (extracts concrete types in order).
  static MonomorphizedKey
  from_bindings(const std::string &name,
                const std::vector<std::string> &type_params,
                const std::unordered_map<std::string, Type> &bindings) {
    std::vector<Type> args;
    for (auto &tp : type_params)
      args.push_back(bindings.at(tp));
    return {name, std::move(args)};
  }

  /// "i32, f64" — comma-separated, no brackets. For error messages and
  /// QualifiedName construction.
  std::string type_args_string() const {
    std::string result;
    for (size_t i = 0; i < type_args.size(); i++) {
      if (i > 0)
        result += ", ";
      result += type_args[i].to_string();
    }
    return result;
  }

  /// Produce a QualifiedName for generic functions/enums/structs.
  sammine_util::QualifiedName
  to_generic_name(const sammine_util::QualifiedName &base) const {
    return base.with_type_args("<" + type_args_string() + ">");
  }

  /// Produce a QualifiedName for typeclass instance methods.
  sammine_util::QualifiedName
  to_typeclass_name(const std::string &method) const {
    return sammine_util::QualifiedName::from_parts(
        std::vector<sammine_util::QualifiedName::Part>{
            {name, "<" + type_args_string() + ">"}, {method, ""}});
  }
};
struct MonomorphizedKeyHash {
  size_t operator()(const MonomorphizedKey &k) const {
    size_t h = std::hash<std::string>{}(k.name);
    std::hash<Type> th;
    for (auto &t : k.type_args)
      hash_combine(h, th(t));
    return h;
  }
};

using TypeBindings = std::unordered_map<std::string, Type>;

std::optional<std::string> incompatibility_hint(const Type &expected,
                                                const Type &actual);

/// The error for a value of `type`, which has an open type argument nothing
/// decided, as `let x = None;` leaves Option<_>. `variant` names the variant
/// written without its type arguments, if the value is one.
std::string open_type_message(const Type &type,
                              const std::optional<std::string> &variant);

inline constexpr std::array<std::string_view, 10> kBuiltinTypeNames = {
    "i32", "i64", "f32", "f64", "bool", "char", "u32", "u64", "string", "unit"};

inline bool is_builtin_type_name(std::string_view name) {
  for (auto b : kBuiltinTypeNames)
    if (b == name)
      return true;
  return false;
}

/// Type lattice for subtyping and compatibility checks.
/// Maps child types to parent types (e.g. Integer→i32, Flt→f64).
/// Used by the type checker to resolve polymorphic literals and validate
/// assignments.
struct TypeMapOrdering {
  std::unordered_map<Type, Type> type_map;

  /// Populate the type lattice with built-in subtype edges
  void populate();

  std::vector<Type> visit_ancestor(const Type &t) const;
  /// The type a and b both have, as an if's branches do: the one the other
  /// flows into, as i64 for a numeric literal and an i64, or Option<i64>
  /// for None and an Option<i64>. Linearity must match: an owning 'ptr<T>
  /// and a ptr<T> have none. nullopt if neither flows into the other.
  std::optional<Type> lowest_common_type(const Type &a, const Type &b) const;

  /// Full check: structure + qualifiers (use for assignments, args, returns)
  bool compatible_to_from(const Type &to, const Type &from) const;

  /// Structure only: ignores linearity (use for if/case arm unification)
  bool structurally_compatible(const Type &to, const Type &from) const;

  /// Qualifier check only: linearity
  bool qualifier_compatible(const Type &to, const Type &from) const;

  /// Call arguments only: an owned 'ptr<T> can be lent to a ptr<T> parameter.
  /// The caller keeps ownership; the callee only reads through it.
  bool lendable_to_from(const Type &to, const Type &from) const;
};
