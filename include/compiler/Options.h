#pragma once
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

class Options {
public:
  std::string file_arg, str_arg;
  GPUMode gpu;
  bool check;
  bool jit;
  std::vector<std::string> jit_args;
  std::string llvm_ir;
  bool mlir_ir = false;
  bool ast_ir = false;
  std::string diagnostics = "none";
  std::string time_val;
  std::filesystem::path output_dir;
  std::vector<std::string> import_paths;
  LibFormat lib_format;
  std::filesystem::path stdlib_dir;
  std::string argv0;
  Options() = delete;
  Options(int argc, char *argv[]);


  bool has_error = false;
  bool from_string = false; 
  void infer(const std::string &output_dir_raw); 
};
}
