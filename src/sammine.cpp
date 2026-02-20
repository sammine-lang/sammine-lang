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
  g_diag
      .add_argument("", "--llvm-ir") // TODO: Somehow make the internal compiler
      .default_value(std::string("false"))
      .implicit_value(std::string("true"))
      .help("sammine compiler spits out LLVM-IR to stdout");
  g_diag.add_argument("", "--ast-ir")
      .default_value(std::string("false"))
      .implicit_value(std::string("true"))
      .help("sammine compiler spits out the internal AST to stdout");
  g_diag.add_argument("", "--diagnostics")
      .default_value(std::string("none"))
      .help("sammine compiler spits out diagnostics for "
            "sammine-lang developers. Use "
            "with value for logging: --diagnostics=stages;lexer;parser. Default value is none");
  g_diag.add_argument("", "--time")
      .default_value(std::string("false"))
      .implicit_value(std::string("simple"))
      .nargs(0, 1)
      .help("Print compilation timing. Also accepts: sparse (per-phase table), coarse (per-phase + all LLVM passes)");
  g_diag.add_argument("", "--dev")
      .default_value(false)
      .implicit_value(true)
      .help("Show compiler source locations in error messages (for developers)");

  if (argc < 1) {
    std::cerr << program;
    return 1;
  }
  try {
    program.parse_args(argc, argv); // Example: ./main -abc 1.95 2.47
    compiler_options[compiler_option_enum::FILE] =
        program.present("-f") ? program.get("-f") : "";
    compiler_options[STR] =
        program.present("-s") ? program.get("-s") : "";
    compiler_options[LLVM_IR] = program.get("--llvm-ir");
    compiler_options[AST_IR] = program.get("--ast-ir");
    compiler_options[DIAGNOSTIC] =
        program.get("--diagnostics");
    compiler_options[CHECK] = program.get("--check");
    compiler_options[DEV] = program.get<bool>("--dev") ? "true" : "false";
    if (program.is_used("--time")) {
      auto val = program.get("--time");
      compiler_options[TIME] = (val == "false") ? "simple" : val;
    } else {
      compiler_options[TIME] = "false";
    }
  } catch (const std::exception &err) {
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_red),
               "Error while parsing arguments\n");
    std::cerr << err.what() << std::endl;
    std::cerr << program;
    return 1;
  }

  CompilerRunner::run(compiler_options);
  return 0;
}
