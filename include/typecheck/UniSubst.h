
#include "typecheck/Types.h"
#include <unordered_map>
namespace sammine_lang::AST {

Type substitute(const Type &type,
                const TypeBindings &bindings);

bool unify(
    const Type &pattern, const Type &concrete,
    std::unordered_map<std::string, Type> &bindings);

}
