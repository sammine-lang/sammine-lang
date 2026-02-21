#pragma once
#include <string>

//! \file Logging.h
//! \brief LLVM-style debug logging infrastructure
//!
//! This file provides LLVM-style debug logging capabilities where each source
//! file can define DEBUG_TYPE to categorize its debug output, and users can
//! enable specific debug types via the --diagnostics CLI flag.
//!
//! Usage:
//!   1. Define DEBUG_TYPE at the top of your source file:
//!      #define DEBUG_TYPE "lexer"
//!      #include "util/Logging.h"
//!
//!   2. Use LOG macro to emit debug output:
//!      LOG({ fmt::print(stderr, "[{}] message\n", DEBUG_TYPE); });
//!
//!   3. Enable specific debug types from command line:
//!      --diagnostics="lexer;parser"

namespace sammine_log {
//! Set the enabled debug types from command line (semicolon-separated)
void set_enabled_types(const std::string &types_str);

//! Get the enabled debug types string
const std::string &get_enabled_types();

//! Check if type is in semicolon-separated list
bool is_type_in_list(const char *type, const std::string &list);
} // namespace sammine_log

//! LOG - Conditionally execute debug logging code
//!
//! Example:
//!   LOG({ fmt::print(stderr, "[lexer] Token count: {}\n", count); });
//!
//! Note: DEBUG_TYPE must be defined before using this macro.
#define LOG(X)                                                                 \
  do {                                                                         \
    if (::sammine_log::is_type_in_list(DEBUG_TYPE,                             \
                                       ::sammine_log::get_enabled_types())) {  \
      X;                                                                       \
    }                                                                          \
  } while (false)
