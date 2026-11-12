
#include "typecheck/Types.h"
#include <unordered_map>
namespace sammine_lang::AST {

/// Whether `type` mentions the type parameter `param_name`.
bool contains_type_param(const Type &type, const std::string &param_name);

Type substitute(const Type &type, const TypeBindings &bindings);

bool unify(const Type &pattern, const Type &concrete,
           std::unordered_map<std::string, Type> &bindings);

} // namespace sammine_lang::AST
