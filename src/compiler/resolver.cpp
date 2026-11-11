
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
                       std::vector<std::string> &extra_object_files) {
  // Every module imports the prelude, except the prelude itself.
  std::string prelude_name(AST::NameResolver::kPrelude);
  auto prelude = find_mn(prelude_name, source_dir);
  std::error_code ec;
  if (!prelude.empty() &&
      !std::filesystem::equivalent(prelude, options_.file_arg, ec)) {
    import_module(prelude_name, source_dir, {}, /*is_transitive=*/false,
                  programAST, extra_object_files);
    if (has_errors())
      return;
  }

  for (auto &import : programAST.imports) {
    import_module(import.module_name, source_dir, import.location,
                  /*is_transitive=*/false, programAST, extra_object_files);
    if (has_errors())
      return;
  }
}

// Recursively import a module's definitions into `programAST`.
//   - Direct imports: pull exported defs + all generic defs + link artifact.
//   - Transitive imports: only pull generic function defs and type defs
//     needed by the monomorphizer — no link artifact.
bool Resolver::import_module(const std::string &name, const Path &src_dir,
                             const sammine_util::Location &loc,
                             bool is_transitive, AST::ProgramAST &programAST,
                             std::vector<std::string> &extra_object_files) {
  if (imported_modules.contains(name))
    return true;
  imported_modules.insert(name);

  auto mn_path = find_mn(name, src_dir);
  if (mn_path.empty()) {
    if (!is_transitive) {
      auto msg = fmt::format("Cannot find module '{}.mn'", name);
      if (reporter.has_value())
        reporter->get().immediate_error(msg, loc);
      add_error(loc, msg);
    }
    return false;
  }

  // Parse the .mn file with SourceInfo for cross-file error locations.
  auto mn_program = get_program_from_file(mn_path, name, loc);
  if (!mn_program)
    return false;

  programAST.module_aliases[name] = AST::import_aliases(mn_program->imports, name);

  // Recursively import this module's own dependencies (transitive).
  auto mod_src_dir = mn_path.parent_path();
  for (auto &sub_import : mn_program->imports) {
    import_module(sub_import.module_name, mod_src_dir, sub_import.location,
                  /*is_transitive=*/true, programAST, extra_object_files);
  }

  // Filter definitions and inject into programAST.
  inject_definitions(mn_program->DefinitionVec, programAST.DefinitionVec, name,
                     is_transitive);

  // Track linkable artifact (only for direct imports).
  if (!is_transitive)
    traceLinkableArtifacts(name, src_dir, extra_object_files);

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
    const std::string &name,
    bool is_transitive) {
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

    // A transitive import only brings what this program compiles of the
    // module: generic functions and types.
    if (is_transitive &&
        !llvm::isa<AST::FuncDefAST, AST::StructDefAST, AST::EnumDefAST,
                   AST::TypeAliasDefAST>(def.get()))
      continue;

    if (auto *fd = llvm::dyn_cast<AST::FuncDefAST>(def.get())) {
      if (!fd->Prototype->is_generic()) {
        if (is_transitive)
          continue;
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
void Resolver::traceLinkableArtifacts(
    const std::string &name, const Path &src_dir,
    std::vector<std::string> &extra_object_files) {
  std::vector<std::string> lib_exts =
      options_.lib_format == LibFormat::Static
          ? std::vector<std::string>{".a", ".so"}
          : std::vector<std::string>{".so", ".a"};
  Path obj_path;
  auto find_lib = [&name, &obj_path,
                   &lib_exts](const Path &dir) -> bool {
    for (auto &ext : lib_exts) {
      auto candidate = dir / (name + ext);
      if (std::filesystem::exists(candidate)) {
        obj_path = candidate;
        return true;
      }
    }
    return false;
  };
  find_lib(".");
  if (obj_path.empty()) {
    for (auto &ipath : options_.import_paths)
      if (find_lib(ipath))
        break;
  }
  if (obj_path.empty())
    find_lib(src_dir);
  if (obj_path.empty() && !options_.stdlib_dir.empty())
    find_lib(options_.stdlib_dir);
  if (!obj_path.empty())
    extra_object_files.push_back(obj_path.string());
}
}; // namespace sammine_lang
