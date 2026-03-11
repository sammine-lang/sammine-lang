//
// Created by jjasmine on 3/7/24.
//
//! \file sammine.cpp
//! \brief The main file to produce an executable that is the sammine compiler

#include "compiler/Compiler.h"
#include <CLI/CLI.hpp>
#include <cpptrace/cpptrace.hpp>
#include <csignal>
#include <cstdlib>
void handler(int sig) {
  // print out all the frames to stderr
  fprintf(stderr,
          "Something has gone terribly wrong in the sammine-lang compiler.");
  fprintf(stderr, "An error has occured outside of sammine_util::abort() and "
                  "Reporter::report()");
  fprintf(stderr, "Error: signal %d:\n", sig);
  cpptrace::generate_trace().print_with_snippets();
  exit(1);
}
using sammine_lang::compiler_option_enum;
namespace co = sammine_lang; // short alias for enum access

int main(int argc, char *argv[]) {
  signal(SIGSEGV, handler);
  CLI::App app{"sammine"};

  std::map<compiler_option_enum, std::string> compiler_options;

  std::string file_arg, str_arg;
  auto *input_group = app.add_option_group("Input");
  input_group->add_option("-f,--file", file_arg,
                          "An input file for compiler to scan over.");
  input_group->add_option("-s,--str", str_arg,
                          "An input string for compiler to scan over.");
  input_group->require_option(1); // exactly one of -f or -s

  bool check = false;
  app.add_flag("--check", check, "Performs compiler check only, no codegen");

  std::string llvm_ir;
  app.add_option("--llvm-ir", llvm_ir,
                 "Emit LLVM IR. Required value: pre, post, or diff")
      ->check(CLI::IsMember({"pre", "post", "diff"}));

  bool mlir_ir = false;
  app.add_flag("--mlir-ir", mlir_ir, "Dump MLIR before lowering to LLVM IR");

  bool ast_ir = false;
  app.add_flag("--ast-ir", ast_ir,
               "sammine compiler spits out the internal AST to stdout");

  std::string diagnostics = "none";
  app.add_option("--diagnostics", diagnostics,
                 "Diagnostics for sammine-lang developers. "
                 "Values: stages;lexer;parser or 'dev' for source locations.")
      ->expected(0, 1)
      ->default_str("dev");

  std::string time_val;
  app.add_option("--time", time_val,
                 "Print compilation timing. Values: simple, sparse, coarse")
      ->check(CLI::IsMember({"simple", "sparse", "coarse"}))
      ->expected(0, 1)
      ->default_str("simple");

  std::string output_dir;
  app.add_option("-O", output_dir,
                 "Output directory for build artifacts (.so, .a, .exe).");

  std::vector<std::string> import_paths;
  app.add_option("-I", import_paths,
                 "Add directory to import search path (repeatable).");

  std::string lib_format;
  app.add_option("--lib", lib_format,
                 "Emit library output. Values: static (.a) or shared (.so)")
      ->check(CLI::IsMember({"static", "shared"}))
      ->expected(0, 1)
      ->default_str("shared");

  CLI11_PARSE(app, argc, argv);

  compiler_options[co::FILE] = file_arg;
  compiler_options[co::STR] = str_arg;
  compiler_options[co::LLVM_IR] = llvm_ir.empty() ? "false" : llvm_ir;
  compiler_options[co::AST_IR] = ast_ir ? "true" : "false";
  compiler_options[co::MLIR_IR] = mlir_ir ? "true" : "false";
  compiler_options[co::DIAGNOSTIC] = diagnostics;
  compiler_options[co::CHECK] = check ? "true" : "false";
  compiler_options[co::TIME] = time_val.empty() ? "false" : time_val;
  compiler_options[co::ARGV0] = argv[0];
  compiler_options[co::OUTPUT_DIR] = output_dir;
  {
    std::string joined;
    for (size_t i = 0; i < import_paths.size(); i++) {
      if (i > 0)
        joined += ";";
      joined += import_paths[i];
    }
    compiler_options[co::IMPORT_PATHS] = joined;
  }
  compiler_options[co::LIB_FORMAT] = lib_format;

  return sammine_lang::CompilerRunner::run(compiler_options);
}
