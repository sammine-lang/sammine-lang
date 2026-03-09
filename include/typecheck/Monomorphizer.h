#pragma once

#include "ast/Ast.h"
#include "typecheck/Types.h"
#include "util/MonomorphizedName.h"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace sammine_lang::AST {

class Monomorphizer {
public:
  using SubstitutionMap = std::unordered_map<std::string, Type>;

  // --- Registration (called during type-check passes 1-2) ---
  void register_generic_func(const std::string &mangled, FuncDefAST *def);
  void register_generic_enum(const std::string &mangled, EnumDefAST *def);
  void register_generic_struct(const std::string &mangled, StructDefAST *def);

  // --- Lookup ---
  FuncDefAST *find_generic_func(const std::string &mangled);
  EnumDefAST *find_generic_enum(const std::string &mangled);
  StructDefAST *find_generic_struct(const std::string &mangled);

  // --- Instantiate ---
  // Functions: dedup'd via internal set, returns nullptr if already done.
  // Enums/Structs: always clone (callers dedup via get_typename_type).
  FuncDefAST *try_instantiate_func(FuncDefAST *generic,
                                   const sammine_util::MonomorphizedName &mono,
                                   const SubstitutionMap &bindings);
  EnumDefAST *instantiate_enum(EnumDefAST *generic,
                               const sammine_util::MonomorphizedName &mono,
                               const SubstitutionMap &bindings);
  StructDefAST *instantiate_struct(StructDefAST *generic,
                                   const sammine_util::MonomorphizedName &mono,
                                   const SubstitutionMap &bindings);

  // --- Output (accessed by Compiler.cpp after type-checking) ---
  std::vector<std::unique_ptr<FuncDefAST>> monomorphized_defs;
  std::vector<std::unique_ptr<EnumDefAST>> monomorphized_enum_defs;
  std::vector<std::unique_ptr<StructDefAST>> monomorphized_struct_defs;

private:
  // Registration maps
  std::unordered_map<std::string, FuncDefAST *> generic_func_defs_;
  std::unordered_map<std::string, EnumDefAST *> generic_enum_defs_;
  std::unordered_map<std::string, StructDefAST *> generic_struct_defs_;

  // Dedup set (functions only — enum/struct dedup via get_typename_type
  // at call sites, which works because they register at root scope)
  std::set<std::string> instantiated_functions_;

  // Cloning internals
  const SubstitutionMap *bindings_ = nullptr;
  std::string resolve_type_name(const std::string &name) const;
  std::unique_ptr<TypeExprAST> clone_type_expr(TypeExprAST *expr);
  std::unique_ptr<TypedVarAST> clone_typed_var(TypedVarAST *var);
  std::unique_ptr<PrototypeAST>
  clone_prototype(PrototypeAST *proto,
                  const sammine_util::MonomorphizedName &mono_name);
  std::unique_ptr<BlockAST> clone_block(BlockAST *block);
  std::unique_ptr<ExprAST> clone_expr(ExprAST *expr);
  std::vector<std::unique_ptr<ExprAST>>
  clone_expr_vec(const std::vector<std::unique_ptr<ExprAST>> &exprs);

  // Internal clone helpers that set bindings_ for the duration
  std::unique_ptr<FuncDefAST>
  clone_func(FuncDefAST *generic,
             const sammine_util::MonomorphizedName &mono_name,
             const SubstitutionMap &bindings);
  std::unique_ptr<EnumDefAST>
  clone_enum(EnumDefAST *generic,
             const sammine_util::MonomorphizedName &mono_name,
             const SubstitutionMap &bindings);
  std::unique_ptr<StructDefAST>
  clone_struct(StructDefAST *generic,
               const sammine_util::MonomorphizedName &mono_name,
               const SubstitutionMap &bindings);
};

} // namespace sammine_lang::AST
