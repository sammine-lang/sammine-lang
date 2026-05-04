#include "compiler/Compiler.h"
#include "compiler/Options.h"
#include "fmt/color.h"
#include "fmt/core.h"
#include "util/Logging.h"
#include "util/Utilities.h"

#include <cstdlib>
#include <filesystem>

#define DEBUG_TYPE "stages"

namespace sammine_lang {

std::string LibraryEmitter::output_path(const std::string &filename) const {
  if (options_.output_dir.empty())
    return filename;
  return (options_.output_dir / filename).string();
}

// Entry point: mirror Compiler::parse() — dispatch by lib_format, then clean
// up the intermediate .o unconditionally (matches pre-refactor behavior).
void LibraryEmitter::Emit() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit library stage...\n");
  });

  autoDetectRuntimeObject();

  switch (options_.lib_format) {
  case LibFormat::Static:
    emitArchive();
    break;
  case LibFormat::Shared:
    emitShared();
    break;
  }

  // Clean up intermediate .o — only the .a/.so is the deliverable.
  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
  std::filesystem::remove(output_path(stem + ".o"));
}

void LibraryEmitter::autoDetectRuntimeObject() {
  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
  std::string runtime_obj = stem + "_runtime.o";

  if (!options_.output_dir.empty()) {
    auto candidate = options_.output_dir / runtime_obj;
    if (std::filesystem::exists(candidate)) {
      extra_link_objs_.push_back(candidate.string());
      return;
    }
  }

  auto source_dir = std::filesystem::path(options_.file_arg).parent_path();
  if (source_dir.empty())
    source_dir = ".";
  auto candidate = source_dir / runtime_obj;
  if (std::filesystem::exists(candidate))
    extra_link_objs_.push_back(candidate.string());
}

void LibraryEmitter::emitArchive() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit archive stage...\n");
  });

  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
  std::string obj_file = output_path(stem + ".o");
  std::string archive_file = output_path(stem + ".a");

  // Collect all .o files into a temp directory, then create the archive.
  // For .a deps, extract their members first so we produce a flat archive.
  auto tmp_dir =
      std::filesystem::temp_directory_path() / ("sammine_ar_" + stem);
  std::filesystem::create_directories(tmp_dir);

  // Copy this module's .o
  std::filesystem::copy_file(
      obj_file, tmp_dir / std::filesystem::path(obj_file).filename(),
      std::filesystem::copy_options::overwrite_existing);

  auto copy_dep = [&](const std::string &dep) {
    std::string ext = std::filesystem::path(dep).extension().string();
    if (ext == ".a") {
      auto abs_dep = std::filesystem::absolute(dep).string();
      std::string cmd =
          fmt::format("cd {} && ar x {}", tmp_dir.string(), abs_dep);
      (void)std::system(cmd.c_str());
    } else {
      std::filesystem::copy_file(
          dep, tmp_dir / std::filesystem::path(dep).filename(),
          std::filesystem::copy_options::overwrite_existing);
    }
  };

  for (auto &dep : extra_object_files_)
    copy_dep(dep);
  for (auto &dep : extra_link_objs_)
    copy_dep(dep);

  // Create the archive from all collected .o files
  auto abs_archive = std::filesystem::absolute(archive_file).string();
  std::string cmd =
      fmt::format("cd {} && ar rcs {} *.o", tmp_dir.string(), abs_archive);
  int result = std::system(cmd.c_str());
  if (result != 0) {
    add_error(sammine_util::Location::NonPrintable(),
              fmt::format("Failed to create archive {}", archive_file));
  }

  std::filesystem::remove_all(tmp_dir);
}

void LibraryEmitter::emitShared() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit shared library stage...\n");
  });

  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
  std::string obj_file = output_path(stem + ".o");
  std::string lib_file = output_path(stem + ".so");

  std::string extra;
  for (auto &obj : extra_link_objs_)
    extra += " " + obj;
  std::string platform_flags;
#ifdef __APPLE__
  platform_flags = " -undefined dynamic_lookup";
#endif
  std::string command = fmt::format("clang++ -shared{} -o {} {}{}",
                                    platform_flags, lib_file, obj_file, extra);
  int result = std::system(command.c_str());
  if (result != 0) {
    add_error(sammine_util::Location::NonPrintable(),
              fmt::format("Failed to create shared library {}", lib_file));
  }
}

} // namespace sammine_lang
