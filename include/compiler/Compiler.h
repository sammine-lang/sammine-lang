//
// Created by Jasmine Tang on 3/27/24.
//

#pragma once
#include <map>
#include <string>
//! \file Compiler.h
//! \brief Define the Compiler staging
namespace sammine_lang {
enum compiler_option_enum {
  FILE,
  STR,
  LLVM_IR,
  AST_IR,
  DIAGNOSTIC,
  CHECK,
  TIME,
  ARGV0,
  BACKEND,
  MLIR_IR,
  OUTPUT_DIR,
  IMPORT_PATHS,
  LIB_FORMAT,
  EXTRA_LINK_OBJS,
};

enum class LibFormat {
  None,   // No library output (.o only, or link to .exe)
  Static, // Emit .a archive
  Shared, // Emit .so shared library
};

class CompilerRunner {
public:
  static void
  run(std::map<compiler_option_enum, std::string> &compiler_options);
};
} // namespace sammine_lang
