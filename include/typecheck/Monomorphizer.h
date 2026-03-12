#pragma once

#include "ast/Ast.h"
#include "typecheck/Types.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sammine_lang::AST {

template <typename T>
struct GenericRegistry {
  std::unordered_map<std::string, T *> defs;
  void register_def(const std::string &mangled, T *def) { defs[mangled] = def; }
  T *find(const std::string &mangled) const {
    auto it = defs.find(mangled);
    return it != defs.end() ? it->second : nullptr;
  }
};

class Monomorphizer {
public:

  // --- Registration & Lookup ---
  GenericRegistry<FuncDefAST> generic_funcs;
  GenericRegistry<EnumDefAST> generic_enums;
  GenericRegistry<StructDefAST> generic_structs;

  // --- Instantiate ---
  // Functions: dedup'd via internal set, returns nullptr if already done.
  // Enums/Structs: always clone (callers dedup via get_typename_type).
  FuncDefAST *try_instantiate_func(FuncDefAST *generic,
                                   const MonomorphizedKey &key,
                                   const TypeBindings &bindings);
  EnumDefAST *instantiate_enum(EnumDefAST *generic,
                               const MonomorphizedKey &key,
                               const TypeBindings &bindings);
  StructDefAST *instantiate_struct(StructDefAST *generic,
                                   const MonomorphizedKey &key,
                                   const TypeBindings &bindings);

  // --- Output (accessed by Compiler.cpp after type-checking) ---
  std::vector<std::unique_ptr<FuncDefAST>> monomorphized_defs;
  std::vector<std::unique_ptr<EnumDefAST>> monomorphized_enum_defs;
  std::vector<std::unique_ptr<StructDefAST>> monomorphized_struct_defs;

private:
  // Dedup set (functions only — enum/struct dedup via get_typename_type
  // at call sites, which works because they register at root scope)
  std::unordered_set<MonomorphizedKey, MonomorphizedKeyHash> instantiated_functions_;

  // Cloning internals
  const TypeBindings *bindings_ = nullptr;
  std::string resolve_type_name(const std::string &name) const;
  std::unique_ptr<TypeExprAST> clone_type_expr(TypeExprAST *expr);
  std::unique_ptr<TypedVarAST> clone_typed_var(TypedVarAST *var);
  std::unique_ptr<PrototypeAST>
  clone_prototype(PrototypeAST *proto,
                  const sammine_util::QualifiedName &new_name);
  std::unique_ptr<BlockAST> clone_block(BlockAST *block);
  std::unique_ptr<ExprAST> clone_expr(ExprAST *expr);
  std::vector<std::unique_ptr<ExprAST>>
  clone_expr_vec(const std::vector<std::unique_ptr<ExprAST>> &exprs);

  // Internal clone helpers that set bindings_ for the duration
  std::unique_ptr<FuncDefAST>
  clone_func(FuncDefAST *generic,
             const sammine_util::QualifiedName &new_name,
             const TypeBindings &bindings);
  std::unique_ptr<EnumDefAST>
  clone_enum(EnumDefAST *generic,
             const sammine_util::QualifiedName &new_name,
             const TypeBindings &bindings);
  std::unique_ptr<StructDefAST>
  clone_struct(StructDefAST *generic,
               const sammine_util::QualifiedName &new_name,
               const TypeBindings &bindings);
};

} // namespace sammine_lang::AST
