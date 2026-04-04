#include "compiler/Compiler.h"


sammine_lang::Options::Options(CLI::App& app) {
  auto *input_group = app.add_option_group("Input");
  input_group->add_option("-f,--file", file_arg,
                          "An input file for compiler to scan over.");
  input_group->add_option("-s,--str", str_arg,
                          "An input string for compiler to scan over.");
  input_group->require_option(1); // exactly one of -f or -s

  check = false;
  app.add_flag("--check", check, "Performs compiler check only, no codegen");

  jit = false;
  app.add_flag("--jit", jit,
               "JIT execute the program directly (only effective with main function)");

  app.add_option("--gpu", gpu,
                 "Lower kernel functions to GPU code. Values: cuda, amd")
      ->check(CLI::IsMember({"cuda", "amd"}));

  app.add_option("--jit-args", jit_args,
                 "Arguments to pass to the JIT-executed program (repeatable).");

  app.add_option("--llvm-ir", llvm_ir,
                 "Emit LLVM IR. Required value: pre, post, or diff")
      ->check(CLI::IsMember({"pre", "post", "diff"}));

  app.add_flag("--mlir-ir", mlir_ir, "Dump MLIR before lowering to LLVM IR");

  app.add_flag("--ast-ir", ast_ir,
               "sammine compiler spits out the internal AST to stdout");

  app.add_option("--diagnostics", diagnostics,
                 "Diagnostics for sammine-lang developers. "
                 "Values: stages;lexer;parser or 'dev' for source locations.")
      ->expected(0, 1)
      ->default_str("dev");

  app.add_option("--time", time_val,
                 "Print compilation timing. Values: simple, sparse, coarse")
      ->check(CLI::IsMember({"simple", "sparse", "coarse"}))
      ->expected(0, 1)
      ->default_str("simple");

  app.add_option("-O", output_dir,
                 "Output directory for build artifacts (.so, .a, .exe).");

  app.add_option("-I", import_paths,
                 "Add directory to import search path (repeatable).");

  app.add_option("--lib", lib_format,
                 "Emit library output. Values: static (.a) or shared (.so)")
      ->check(CLI::IsMember({"static", "shared"}))
      ->expected(0, 1)
      ->default_str("shared");
}
