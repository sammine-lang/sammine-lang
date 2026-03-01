//
// Created by jjasmine on 3/7/24.
//
//! \file sammine.cpp
//! \brief The main file to produce an executable that is the sammine compiler

#include "compiler/Compiler.h"
#include "fmt/color.h"
#include "util/Utilities.h"
#include <argparse/argparse.hpp>
#include <cpptrace/cpptrace.hpp>
#include <csignal>
#include <cstdlib>
using sammine_lang::compiler_option_enum;

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
using namespace sammine_lang;
int main(int argc, char *argv[]) {
  signal(SIGSEGV, handler);
  argparse::ArgumentParser program("sammine");

  std::map<compiler_option_enum, std::string> compiler_options;

  auto &gi = program.add_mutually_exclusive_group(true);
  gi.add_argument("-f", "--file")
      .help("An input file for compiler to scan over.");
  gi.add_argument("-s", "--str")
      .help("An input string for compiler to scan over.");

  program.add_argument("--check")
      .help("Performs compiler check only, no codegen")
      .default_value(std::string("false"))
      .implicit_value(std::string("true"));

  auto &g_diag = program.add_group("Diagnostics related options");
  g_diag.add_argument("", "--llvm-ir")
      .default_value(std::string("false"))
      .nargs(1)
      .help("Emit LLVM IR. Required value: pre, post, or diff");
  g_diag.add_argument("", "--mlir-ir")
      .default_value(std::string("false"))
      .implicit_value(std::string("true"))
      .help("Dump MLIR before lowering to LLVM IR");
  g_diag.add_argument("", "--ast-ir")
      .default_value(std::string("false"))
      .implicit_value(std::string("true"))
      .help("sammine compiler spits out the internal AST to stdout");
  g_diag.add_argument("", "--diagnostics")
      .default_value(std::string("none"))
      .help("sammine compiler spits out diagnostics for "
            "sammine-lang developers. Use "
            "with value for logging: --diagnostics=stages;lexer;parser. "
            "Use 'dev' to show compiler source locations in error messages. "
            "Default value is none");
  g_diag.add_argument("", "--time")
      .default_value(std::string("false"))
      .implicit_value(std::string("simple"))
      .nargs(0, 1)
      .help("Print compilation timing. Also accepts: sparse (per-phase table), "
            "coarse (per-phase + all LLVM passes)");

  program.add_argument("-O")
      .default_value(std::string(""))
      .help("Output directory for build artifacts (.o, .exe, .mni). Defaults to current directory.");

  program.add_argument("-I")
      .default_value(std::vector<std::string>{})
      .append()
      .help("Add directory to import search path (repeatable).");

  program.add_argument("--lib")
      .default_value(std::string(""))
      .implicit_value(std::string("shared"))
      .nargs(0, 1)
      .help("Emit library output instead of executable. Use --lib=static for "
            "a .a archive, or --lib (no value) for a .so shared library.");

  if (argc < 1) {
    std::cerr << program;
    return 1;
  }
  try {
    program.parse_args(argc, argv); // Example: ./main -abc 1.95 2.47
    compiler_options[compiler_option_enum::FILE] =
        program.present("-f") ? program.get("-f") : "";
    compiler_options[STR] = program.present("-s") ? program.get("-s") : "";
    if (program.is_used("--llvm-ir")) {
      auto val = program.get("--llvm-ir");
      if (val != "pre" && val != "post" && val != "diff") {
        fmt::print(stderr,
                   sammine_util::styled(fmt::terminal_color::bright_red),
                   "Error: --llvm-ir requires a value: pre, post, or diff\n");
        std::cerr << program;
        return 1;
      }
      compiler_options[LLVM_IR] = val;
    } else {
      compiler_options[LLVM_IR] = "false";
    }
    compiler_options[AST_IR] = program.get("--ast-ir");
    compiler_options[MLIR_IR] = program.get("--mlir-ir");
    compiler_options[DIAGNOSTIC] = program.get("--diagnostics");
    compiler_options[CHECK] = program.get("--check");
    if (program.is_used("--time")) {
      auto val = program.get("--time");
      compiler_options[TIME] = (val == "false") ? "simple" : val;
    } else {
      compiler_options[TIME] = "false";
    }
    compiler_options[BACKEND] = "mlir";
  } catch (const std::exception &err) {
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_red),
               "Error while parsing arguments\n");
    std::cerr << err.what() << std::endl;
    std::cerr << program;
    return 1;
  }

  compiler_options[ARGV0] = argv[0];
  compiler_options[OUTPUT_DIR] = program.get("-O");
  {
    auto ipaths = program.get<std::vector<std::string>>("-I");
    std::string joined;
    for (size_t i = 0; i < ipaths.size(); i++) {
      if (i > 0)
        joined += ";";
      joined += ipaths[i];
    }
    compiler_options[IMPORT_PATHS] = joined;
  }
  {
    auto lib_val = program.get("--lib");
    if (!lib_val.empty() && lib_val != "static" && lib_val != "shared") {
      fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_red),
                 "Error: --lib only accepts 'static' (or no value for shared)\n");
      std::cerr << program;
      return 1;
    }
    compiler_options[LIB_FORMAT] = lib_val;
  }
  CompilerRunner::run(compiler_options);
  return 0;
}
