#include "CLI/CLI.hpp"
#include "util/Utilities.h"
#include "compiler/Options.h"

namespace sammine_lang {
  static LibFormat parse_lib_format(const std::string &s) {
    if (s == "static")
      return LibFormat::Static;
    if (s == "shared")
      return LibFormat::Shared;
    return LibFormat::Shared;
  }
  static GPUMode parse_gpu_mode(const std::string &s) {
    if (s == "cuda")
      return GPUMode::CUDA;
    if (s == "amd")
      return GPUMode::AMD;
    return GPUMode::NONE;
  }
  // "ast", "mlir", "llvm", "llvm-opt", "llvm-diff"
  static EmitKind parse_emit_kind(const std::string &s) {
    if (s == "ast")
      return EmitKind::Ast;
    if (s == "mlir")
      return EmitKind::Mlir;
    if (s == "llvm")
      return EmitKind::Llvm;
    if (s == "llvm-opt")
      return EmitKind::LlvmOpt;
    if (s == "llvm-diff")
      return EmitKind::LlvmDiff;
    return EmitKind::None;
  }
  // "simple", "sparse", "coarse"
  static TimingMode parse_timing_mode(const std::string &s) {
    if (s == "simple")
      return TimingMode::SIMPLE;
    if (s == "sparse")
      return TimingMode::SPARSE;
    if (s == "coarse")
      return TimingMode::COARSE;
    return TimingMode::NONE;

  }
}
namespace sammine_lang {
Options::Options(int argc, char *argv[]) {
  CLI::App app{"sammine"};
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

  std::string gpu_mode_raw;
  app.add_option("--gpu", gpu_mode_raw,
                 "Lower kernel functions to GPU code. Values: cuda, amd")
      ->check(CLI::IsMember({"cuda", "amd"}));

  app.add_option("--jit-args", jit_args,
                 "Arguments to pass to the JIT-executed program (repeatable).");

  std::string emit_raw;
  app.add_option("--emit", emit_raw,
                 "Print an intermediate form to stdout, then stop. Values: "
                 "ast, mlir, llvm (before optimization), llvm-opt (after), "
                 "llvm-diff")
      ->check(CLI::IsMember({"ast", "mlir", "llvm", "llvm-opt", "llvm-diff"}));

  app.add_flag("--mir", mir,
               "Lower function bodies through MIR where it handles them");

  app.add_option("--diagnostics", diagnostics,
                 "Diagnostics for sammine-lang developers. "
                 "Values: stages;lexer;parser or 'dev' for source locations.")
      ->expected(0, 1)
      ->default_str("dev");

  std::string time_val_raw;
  app.add_option("--time", time_val_raw,
                 "Print compilation timing. Values: simple, sparse, coarse")
      ->check(CLI::IsMember({"simple", "sparse", "coarse"}))
      ->expected(0, 1)
      ->default_str("simple");

  std::string output_dir_raw;
  app.add_option("-O", output_dir_raw,
                 "Output directory for build artifacts (.so, .a, .exe).");

  app.add_option("-I", import_paths,
                 "Add directory to import search path (repeatable).");

  std::string lib_format_raw;
  app.add_option("--lib", lib_format_raw,
                 "Emit library output. Values: static (.a) or shared (.so)")
      ->check(CLI::IsMember({"static", "shared"}))
      ->expected(0, 1)
      ->default_str("shared");

  argv0 = argv[0];
  // CLI11 signals --help, like a bad flag, with an exception. app.exit
  // prints what it asks for and gives the exit code: 0 for --help.
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    exit_code = app.exit(e);
    return;
  }
  lib_format = parse_lib_format(lib_format_raw);
  gpu = parse_gpu_mode(gpu_mode_raw);
  time_val = parse_timing_mode(time_val_raw);
  emit = parse_emit_kind(emit_raw);


  if (!output_dir_raw.empty())
    std::filesystem::create_directories(output_dir_raw);
  infer(output_dir_raw);
}


void Options::infer(const std::string &output_dir_raw) {
    if (!argv0.empty()) {
      std::error_code ec;
      auto bin_path = std::filesystem::canonical(argv0, ec);
      if (!ec)
        stdlib_dir = bin_path.parent_path().parent_path() / "lib" / "sammine";
    }


    if (!this->str_arg.empty()) {
      this->file_arg = "-s|--str";
      this->from_string = true;
    } else if (!this->file_arg.empty()) {
      sammine_util::Reportee reportee;
      auto contents =
          sammine_util::get_string_from_file(reportee, this->file_arg);
      if (!contents) {
        fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
                   "[Error during compiler initial phase]\n");
        sammine_util::Reporter(this->file_arg, "", 0).report(reportee);
        has_error = true;
        return;
      }
      this->str_arg = std::move(*contents);
    } else {
      fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
                 "[Error during compiler initial phase]\n");
      fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
                 "[Both the file name and the string input is empty]\n");
      has_error = true;
      return;
    }

    output_dir = std::filesystem::path(output_dir_raw);
  }
}
