#pragma once

#include "llvm/ADT/StringRef.h"

//! \file Runtime.h
//! \brief The C functions every module declares: for allocation, for
//! formatted output, and for ending the program.

namespace sammine_lang::runtime {

inline constexpr llvm::StringLiteral kMalloc = "malloc";
inline constexpr llvm::StringLiteral kFree = "free";
inline constexpr llvm::StringLiteral kFprintf = "fprintf";
// The prelude's C runtime (stdlib/definitions_runtime.c): C's stdout and
// stderr, and a stream that keeps its text, for format().
inline constexpr llvm::StringLiteral kStdout = "sammine_stdout";
inline constexpr llvm::StringLiteral kStderr = "sammine_stderr";
inline constexpr llvm::StringLiteral kTextOpen = "sammine_text_open";
inline constexpr llvm::StringLiteral kTextStream = "sammine_text_stream";
inline constexpr llvm::StringLiteral kTextFinish = "sammine_text_finish";
inline constexpr llvm::StringLiteral kTextTake = "sammine_text_take";
inline constexpr llvm::StringLiteral kExit = "exit";

} // namespace sammine_lang::runtime
