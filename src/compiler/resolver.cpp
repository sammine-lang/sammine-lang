
#include "compiler/Compiler.h"
#include "compiler/Options.h"
#include "fmt/core.h"
#include "parser/Parser.h"
#include "semantics/NameResolver.h"

namespace sammine_lang {

// Entry point: walk every direct import in `programAST` and recurse into
// transitive imports via `import_module`. Mirrors Parser::Parse() — all
// orchestration lives here so Compiler::resolve_imports is a thin driver.
void Resolver::Resolve(AST::ProgramAST &programAST, const Path &source_dir,
                       std::vector<std::string> &extra_object_files,
                       std::vector<std::string> &system_libraries) {
  traceCLibraries(programAST.DefinitionVec, source_dir);
  // Every module imports the prelude, except the prelude itself.
  std::string prelude_name(AST::NameResolver::kPrelude);
  auto prelude = find_mn(prelude_name, source_dir);
  std::error_code ec;
  if (!prelude.empty() &&
      !std::filesystem::equivalent(prelude, options_.file_arg, ec)) {
    import_module(prelude_name, source_dir, {}, programAST,
                  extra_object_files);
    if (has_errors())
      return;
  }

  for (auto &import : programAST.imports) {
    import_module(import.module_name, source_dir, import.location, programAST,
                  extra_object_files);
    if (has_errors())
      return;
  }

  // C libraries come after every module's library: those are what use them.
  for (auto &library : c_libraries_) {
    if (library.file.empty())
      system_libraries.push_back(library.name);
    else
      extra_object_files.push_back(library.file.string());
  }
}

// Import a module and, recursively, the modules it imports. Every module in
// the graph is imported the same way, whichever reaches it first: all its
// definitions and its library. Which modules a program can name is decided by
// its own import aliases (NameResolver), not by what was imported, so a
// module reached only through another is there for that module's code, such
// as its generic functions compiled here.
bool Resolver::import_module(const std::string &name, const Path &src_dir,
                             const sammine_util::Location &loc,
                             AST::ProgramAST &programAST,
                             std::vector<std::string> &extra_object_files) {
  if (imported_modules.contains(name))
    return true;
  imported_modules.insert(name);

  auto mn_path = find_mn(name, src_dir);
  if (mn_path.empty()) {
    auto msg = fmt::format("Cannot find module '{}.mn'", name);
    if (reporter.has_value())
      reporter->get().immediate_error(msg, loc);
    add_error(loc, msg);
    return false;
  }

  // Parse the .mn file with SourceInfo for cross-file error locations.
  auto mn_program = get_program_from_file(mn_path, name, loc);
  if (!mn_program)
    return false;

  programAST.module_aliases[name] = AST::import_aliases(mn_program->imports, name);

  // Its library comes before its imports' in the link: a static archive's
  // undefined symbols are only looked for in what follows it.
  traceLinkableArtifacts(name, src_dir, extra_object_files);

  auto mod_src_dir = mn_path.parent_path();
  traceCLibraries(mn_program->DefinitionVec, mod_src_dir);
  for (auto &sub_import : mn_program->imports)
    import_module(sub_import.module_name, mod_src_dir, sub_import.location,
                  programAST, extra_object_files);

  inject_definitions(mn_program->DefinitionVec, programAST.DefinitionVec, name);
  return true;
}

// Find a .mn file by searching CWD → -I paths → source_dir → stdlib_dir.
Path Resolver::find_mn(const std::string &name,
                                        const Path &src_dir) {
  Path p = name + ".mn";
  if (std::filesystem::exists(p))
    return p;
  for (auto &ipath : options_.import_paths) {
    auto c = Path(ipath) / (name + ".mn");
    if (std::filesystem::exists(c))
      return c;
  }
  p = src_dir / (name + ".mn");
  if (std::filesystem::exists(p))
    return p;
  if (!options_.stdlib_dir.empty()) {
    p = options_.stdlib_dir / (name + ".mn");
    if (std::filesystem::exists(p))
      return p;
  }
  return {};
}
UP Resolver::get_program_from_file(const Path &file_path,
                                   const std::string &name,
                                   const sammine_util::Location &loc) {

  auto contents =
      sammine_util::get_string_from_file(*this, file_path.string(), loc);
  if (!contents)
    return nullptr;
  std::string mn_input = std::move(*contents);
  auto si = std::make_shared<sammine_util::SourceInfo>(
      sammine_util::SourceInfo{file_path.string(), mn_input});
  Lexer mn_lexer(mn_input, si);
  auto mn_tok_stream = mn_lexer.getTokenStream();
  Parser mn_parser(mn_tok_stream);
  auto mn_program = mn_parser.Parse();

  if (mn_parser.has_errors()) {
    add_error(loc,
              fmt::format("Failed to parse module '{}'", file_path.string()));
    return nullptr;
  }
  return mn_program;
}

void Resolver::inject_definitions(
    std::vector<std::unique_ptr<DefinitionAST>> &DefinitionVec,
    std::vector<std::unique_ptr<DefinitionAST>> &program_vec,
    const std::string &name) {
  // Every definition of the module comes, C bindings included, the ones it
  // doesn't export too, marked by is_exported: NameResolver only lets the
  // module's own code name those, and its code compiled here may. A
  // function's body stays in the module, compiled first, so only its
  // declaration comes. A generic function's body comes, since it is compiled
  // here for the types it is used with, and so do instance methods', as
  // copies to inline (emitted available_externally).
  for (auto it = DefinitionVec.rbegin(); it != DefinitionVec.rend(); ++it) {
    auto &def = *it;
    // NameResolver names it after its module.
    def->defining_module = name;

    if (auto *fd = llvm::dyn_cast<AST::FuncDefAST>(def.get())) {
      if (!fd->Prototype->is_generic()) {
        auto ext = std::make_unique<AST::ExternAST>(std::move(fd->Prototype));
        ext->is_exported = fd->is_exported;
        ext->defining_module = fd->defining_module;
        ext->is_c_function = false;
        def = std::move(ext);
      }
    } else if (auto *tci = llvm::dyn_cast<AST::TypeClassInstanceAST>(def.get())) {
      tci->is_declaration = true;
    }
    program_vec.insert(program_vec.begin(), std::move(def));
  }
}
// Find `<stem>.so` or `<stem>.a` where modules are found: CWD → -I paths →
// `src_dir` → stdlib_dir; the format being built is preferred.
Path Resolver::find_library(const std::string &stem, const Path &src_dir) {
  std::vector<std::string> lib_exts =
      options_.lib_format == LibFormat::Static
          ? std::vector<std::string>{".a", ".so"}
          : std::vector<std::string>{".so", ".a"};
  auto find_in = [&](const Path &dir) -> Path {
    for (auto &ext : lib_exts) {
      auto candidate = dir / (stem + ext);
      if (std::filesystem::exists(candidate))
        return candidate;
    }
    return {};
  };
  std::vector<Path> dirs{"."};
  dirs.insert(dirs.end(), options_.import_paths.begin(),
              options_.import_paths.end());
  dirs.push_back(src_dir);
  if (!options_.stdlib_dir.empty())
    dirs.push_back(options_.stdlib_dir);
  for (auto &dir : dirs)
    if (auto found = find_in(dir); !found.empty())
      return found;
  return {};
}

void Resolver::traceLinkableArtifacts(
    const std::string &name, const Path &src_dir,
    std::vector<std::string> &extra_object_files) {
  if (auto lib = find_library(name, src_dir); !lib.empty())
    extra_object_files.push_back(lib.string());
}

// Each library a `reuse "lib" { ... }` names, once: libSDL3 for "SDL3",
// looked for next to the module that names it, as its library is. One not
// found is the system's, which the linker and loader find themselves.
void Resolver::traceCLibraries(
    const std::vector<std::unique_ptr<AST::DefinitionAST>> &definitions,
    const Path &src_dir) {
  for (auto &def : definitions) {
    auto *ext = llvm::dyn_cast<AST::ExternAST>(def.get());
    if (!ext || ext->library.empty() ||
        std::ranges::any_of(c_libraries_, [&](const CLibrary &library) {
          return library.name == ext->library;
        }))
      continue;
    c_libraries_.push_back(
        {ext->library, find_library("lib" + ext->library, src_dir)});
  }
}
}; // namespace sammine_lang
