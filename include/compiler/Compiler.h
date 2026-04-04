//
// Created by Jasmine Tang on 3/27/24.
//

#pragma once
#include "CLI/CLI.hpp"
#include "util/Utilities.h"
#include <filesystem>
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
  std::filesystem::path output_dir;
  std::vector<std::string> import_paths;
  LibFormat lib_format;
  std::filesystem::path stdlib_dir;
  std::string argv0;
  Options() = delete;
  Options(int argc, char *argv[]);


  bool has_error = false;
  bool from_string = false; 
  void infer(const std::string &output_dir_raw); 
};

class CompilerRunner {
public:
  static int run(const Options &options);
};

} // namespace sammine_lang
