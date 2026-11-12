
#include "typecheck/Types.h"
#include <span>
#include <unordered_map>
namespace sammine_lang::AST {

/// Whether `type` mentions the type parameter `param_name`.
bool contains_type_param(const Type &type, const std::string &param_name);

Type substitute(const Type &type, const TypeBindings &bindings);

bool unify(const Type &pattern, const Type &concrete,
           std::unordered_map<std::string, Type> &bindings);

/// Whether some choice of the type parameters in `a` and `b` makes each
/// a[i] equal to b[i]. The parameters of `a` and of `b` must not share names.
bool unifiable(std::span<const Type> a, std::span<const Type> b);

} // namespace sammine_lang::AST
