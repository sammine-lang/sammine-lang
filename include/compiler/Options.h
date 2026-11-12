#pragma once
#include <optional>
#include <filesystem>
#include <string>
#include <vector>
namespace sammine_lang {
enum class LibFormat {
  Static, // Emit .a archive
  Shared, // Emit .so shared library
};
enum class GPUMode {
  CUDA, 
  AMD,
  NONE,
};
/// What --emit prints to stdout before the compiler stops.
enum class EmitKind {
  None,
  Ast,      // the AST after type checking
  Mlir,     // MLIR before it is lowered to LLVM IR
  Llvm,     // LLVM IR before optimization
  LlvmOpt,  // LLVM IR after optimization
  LlvmDiff, // a unified diff of the two
};
enum class TimingMode {
  SIMPLE, 
  COARSE,
  SPARSE,
  NONE,
};

class Options {
public:
  std::string file_arg, str_arg;
  GPUMode gpu;
  bool check;
  bool jit;
  std::vector<std::string> jit_args;
  EmitKind emit = EmitKind::None;
  std::string diagnostics = "none";
  TimingMode time_val;
  std::filesystem::path output_dir;
  std::vector<std::string> import_paths;
  LibFormat lib_format;
  std::filesystem::path stdlib_dir;
  std::string argv0;
  Options() = delete;
  Options(int argc, char *argv[]);


  bool has_error = false;
  // Set when parsing the command line already finished the run, e.g. with
  // --help: exit with this code without compiling.
  std::optional<int> exit_code;
  bool from_string = false; 
  void infer(const std::string &output_dir_raw); 
};
}
