//
// Created by jjasmine on 3/7/24.
//
//! \file sammine.cpp
//! \brief The main file to produce an executable that is the sammine compiler

#include "compiler/Compiler.h"
#include "fmt/color.h"
#include "llvm/Support/CommandLine.h"
#include <argparse/argparse.hpp>
#include <cpptrace/basic.hpp>
#include <cpptrace/cpptrace.hpp>
#include <csignal>
#include <cstdlib>
#include <unistd.h>
using namespace llvm;
using sammine_lang::compiler_option_enum;

// ---------------------------------------------
// version
// ---------------------------------------------
void printVersion(raw_ostream &out) { out << "Sammine Compiler raw beta\n"; }

// ---------------------------------------------
// primary options
// ---------------------------------------------
static cl::OptionCategory PrimaryCat("Primary");
static cl::opt<std::string>
    InputFile("file", cl::desc("An input file for compiler to scan over"),
              cl::value_desc("filename"), cl::cat(PrimaryCat));

static cl::alias InputFileShort("f", cl::desc("Alias for --file"),
                                cl::aliasopt(InputFile), cl::cat(PrimaryCat));

static cl::opt<std::string>
    InputString("str", cl::desc("An input string for compiler to scan over"),
                cl::value_desc("source"), cl::cat(PrimaryCat));

static cl::alias InputStringShort("s", cl::desc("Alias for --str"),
                                  cl::aliasopt(InputString),
                                  cl::cat(PrimaryCat));

// ---------------------------------------------
// Diagnostics options
// ---------------------------------------------
static cl::OptionCategory DiagnosticCat("Diagnostics");

static cl::opt<bool> EmitAST("ast-ir",
                             cl::desc("Spit out internal AST IR to stdout"),
                             cl::init(false), cl::cat(DiagnosticCat));

static cl::opt<bool> EmitLLVMIR("llvm-ir",
                                cl::desc("Spit out LLVM IR to stdout"),
                                cl::init(false), cl::cat(DiagnosticCat));

static cl::opt<bool>
    EmitDiagnostic("diagnostics",
                   cl::desc("Stage-wise diagnostics (for compiler devs)"),
                   cl::init(false), cl::cat(DiagnosticCat));
static cl::opt<bool>
    CheckOnly("check", cl::desc("Performs compiler check only, no codegen"),
              cl::init(false), cl::cat(DiagnosticCat));

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
  cl::SetVersionPrinter(printVersion);
  cl::HideUnrelatedOptions({&PrimaryCat, &DiagnosticCat});
  cl::ParseCommandLineOptions(argc, argv, "sammine compiler\n");

  if (InputFile.empty()) {
    llvm::errs() << "error: you must pass either -f <file> or -s <source>\n";
    return 1;
  }

  std::map<compiler_option_enum, std::string> opts;

  opts[compiler_option_enum::FILE] = InputFile;
  opts[compiler_option_enum::STR] = InputString;
  opts[compiler_option_enum::CHECK] = CheckOnly ? "true" : "false";
  opts[compiler_option_enum::LLVM_IR] = EmitLLVMIR ? "true" : "false";
  opts[compiler_option_enum::AST_IR] = EmitAST ? "true" : "false";
  opts[compiler_option_enum::DIAGNOSTIC] = EmitDiagnostic ? "true" : "false";

  CompilerRunner::run(opts);
  return 0;
}
