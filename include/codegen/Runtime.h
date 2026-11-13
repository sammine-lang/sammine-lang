#pragma once

#include "llvm/ADT/StringRef.h"

//! \file Runtime.h
//! \brief The C functions every module declares, for allocation and for
//! reporting a failed bounds check.

namespace sammine_lang::runtime {

inline constexpr llvm::StringLiteral kMalloc = "malloc";
inline constexpr llvm::StringLiteral kFree = "free";
inline constexpr llvm::StringLiteral kDprintf = "dprintf";
inline constexpr llvm::StringLiteral kExit = "exit";

} // namespace sammine_lang::runtime
