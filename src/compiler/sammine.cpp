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

int main(int argc, char *argv[]) {
  signal(SIGSEGV, handler);

  sammine_lang::Options options(argc, argv);
  if (options.has_error)
    return 1;

  return sammine_lang::CompilerRunner::run(options);
}
