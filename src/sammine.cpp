//
// Created by jjasmine on 3/7/24.
//
//! \file sammine.cpp
//! \brief The main file to produce an executable that is the sammine compiler

#include "compiler/Compiler.h"
#include "fmt/color.h"
#include <argparse/argparse.hpp>
using sammine_lang::compiler_option_enum;

using namespace sammine_lang;
int main(int argc, char *argv[]) {
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

  auto &g_diag = program.add_group("Diagnostics");
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
      .default_value(std::string("false"))
      .implicit_value(std::string("true"))
      .help("sammine compiler spits out stage-wise diagnostics for "
            "sammine-lang developers");

  if (argc < 1) {
    std::cerr << program;
    return 1;
  }
  try {
    program.parse_args(argc, argv); // Example: ./main -abc 1.95 2.47
    compiler_options[compiler_option_enum::FILE] =
        program.present("-f") ? program.get("-f") : "";
    compiler_options[compiler_option_enum::STR] =
        program.present("-s") ? program.get("-s") : "";
    compiler_options[compiler_option_enum::LLVM_IR] = program.get("--llvm-ir");
    compiler_options[compiler_option_enum::AST_IR] = program.get("--ast-ir");
    compiler_options[compiler_option_enum::DIAGNOSTIC] =
        program.get("--diagnostics");
    compiler_options[compiler_option_enum::CHECK] = program.get("--check");
  } catch (const std::exception &err) {
    fmt::print(stderr, fg(fmt::terminal_color::bright_red),
               "Error while parsing arguments\n");
    std::cerr << err.what() << std::endl;
    std::cerr << program;
    return 1;
  }

  CompilerRunner::run(compiler_options);
  return 0;
}
