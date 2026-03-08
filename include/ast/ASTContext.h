#pragma once
#include "ast/AstDecl.h"
#include <string>
#include <vector>

//! \file ASTContext.h
//! \brief Unified context struct that tracks what enclosing AST nodes we're
//! inside during visitor traversal. Pushed/popped with scope, inheriting
//! from parent on push so inner scopes automatically see outer context.

namespace sammine_lang::AST {

struct ASTContext {
  // Enclosing function (null at top level)
  FuncDefAST *enclosing_function = nullptr;

  // Enclosing kernel (null outside kernels)
  KernelDefAST *enclosing_kernel = nullptr;

  // Import context (used by ScopeGeneratorVisitor)
  bool in_imported_def = false;
  std::string import_module;
  std::vector<std::string> generic_type_params;

  // Convenience queries
  bool in_function() const { return enclosing_function != nullptr; }
  bool in_kernel() const { return enclosing_kernel != nullptr; }
};

} // namespace sammine_lang::AST
