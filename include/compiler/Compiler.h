//
// Created by Jasmine Tang on 3/27/24.
//

#pragma once
#include "CLI/CLI.hpp"
#include <map>
#include <string>
#include <vector>
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
  MLIR_IR,
  OUTPUT_DIR,
  IMPORT_PATHS,
  LIB_FORMAT,
  JIT,
  JIT_ARGS,
  GPU,
};

enum class LibFormat {
  None,   // No library output (.o only, or link to .exe)
  Static, // Emit .a archive
  Shared, // Emit .so shared library
};
class Options {
public:
  std::string file_arg, str_arg;
  std::string gpu;
  bool check;
  bool jit;
  std::vector<std::string> jit_args;
  std::string llvm_ir;
  bool mlir_ir = false;
  bool ast_ir = false;
  std::string diagnostics = "none";
  std::string time_val;
  std::string output_dir;
  std::vector<std::string> import_paths;
  std::string lib_format;

  Options() = delete;
  Options(CLI::App&);
};

class CompilerRunner {
public:
  static int run(const Options &options, const std::map<compiler_option_enum, std::string> &compiler_options);
};

} // namespace sammine_lang
