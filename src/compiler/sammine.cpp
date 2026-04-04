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

  std::map<compiler_option_enum, std::string> compiler_options;
  sammine_lang::Options options(argc, argv);

  compiler_options[co::FILE] = options.file_arg;
  compiler_options[co::STR] = options.str_arg;
  compiler_options[co::LLVM_IR] = options.llvm_ir.empty() ? "false" : options.llvm_ir;
  compiler_options[co::AST_IR] = options.ast_ir ? "true" : "false";
  compiler_options[co::MLIR_IR] = options.mlir_ir ? "true" : "false";
  compiler_options[co::DIAGNOSTIC] = options.diagnostics;
  compiler_options[co::CHECK] = options.check ? "true" : "false";
  compiler_options[co::TIME] = options.time_val.empty() ? "false" : options.time_val;
  compiler_options[co::ARGV0] = argv[0];
  compiler_options[co::OUTPUT_DIR] = options.output_dir;
  {
    std::string joined;
    for (size_t i = 0; i < options.import_paths.size(); i++) {
      if (i > 0)
        joined += ";";
      joined += options.import_paths[i];
    }
    compiler_options[co::IMPORT_PATHS] = joined;
  }
  compiler_options[co::JIT] = options.jit ? "true" : "false";
  compiler_options[co::GPU] = options.gpu;
  {
    std::string joined;
    for (size_t i = 0; i < options.jit_args.size(); i++) {
      if (i > 0)
        joined += ";";
      joined += options.jit_args[i];
    }
    compiler_options[co::JIT_ARGS] = joined;
  }

  return sammine_lang::CompilerRunner::run(options, compiler_options);
}
