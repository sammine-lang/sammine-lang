
#include "compiler/Compiler.h"
#include "compiler/Options.h"
#include "fmt/core.h"
#include "parser/Parser.h"

namespace sammine_lang {

// Entry point: walk every direct import in `programAST` and recurse into
// transitive imports via `import_module`. Mirrors Parser::Parse() — all
// orchestration lives here so Compiler::resolve_imports is a thin driver.
void Resolver::Resolve(AST::ProgramAST &programAST, const Path &source_dir,
                       std::vector<std::string> &extra_object_files) {
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

  std::string mn_input = sammine_util::get_string_from_file(file_path.string());
  auto si = std::make_shared<sammine_util::SourceInfo>(
      sammine_util::SourceInfo{file_path.string(), mn_input});
  Lexer mn_lexer(mn_input, si);
  auto mn_tok_stream = mn_lexer.getTokenStream();
  Parser mn_parser(mn_tok_stream);
  mn_parser.alias_to_module[name] = name;
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
  // Filter definitions and inject into programAST.
  for (auto it = DefinitionVec.rbegin(); it != DefinitionVec.rend(); ++it) {
    auto &def = *it;

    if (auto *fd = llvm::dyn_cast<AST::FuncDefAST>(def.get())) {
      bool is_generic = fd->Prototype->is_generic();
      if (is_transitive && !is_generic)
        continue; // transitive: only need generic defs
      if (!is_transitive && !fd->is_exported && !is_generic)
        continue; // direct: skip non-exported non-generic
      fd->Prototype->functionName =
          fd->Prototype->functionName.with_module(name);

      if (is_generic) {
        program_vec.insert(program_vec.begin(),
                                         std::move(def));
      } else {
        // Non-generic exported → extern
        auto ext = std::make_unique<AST::ExternAST>(std::move(fd->Prototype));
        ext->is_exported = fd->is_exported;
        program_vec.insert(program_vec.begin(),
                                         std::move(ext));
      }
    } else if (auto *ext = llvm::dyn_cast<AST::ExternAST>(def.get())) {
      if (is_transitive || !ext->is_exported)
        continue;
      ext->Prototype->functionName =
          ext->Prototype->functionName.with_module(name);
      program_vec.insert(program_vec.begin(),
                                       std::move(def));
    } else if (auto *sd = llvm::dyn_cast<AST::StructDefAST>(def.get())) {
      if (!sd->is_exported)
        continue;
      sd->struct_name = sd->struct_name.with_module(name);
      program_vec.insert(program_vec.begin(),
                                       std::move(def));
    } else if (auto *ed = llvm::dyn_cast<AST::EnumDefAST>(def.get())) {
      if (!ed->is_exported)
        continue;
      ed->enum_name = ed->enum_name.with_module(name);
      program_vec.insert(program_vec.begin(),
                                       std::move(def));
    } else if (auto *ta = llvm::dyn_cast<AST::TypeAliasDefAST>(def.get())) {
      if (!ta->is_exported)
        continue;
      program_vec.insert(program_vec.begin(),
                                       std::move(def));
    } else if (llvm::isa<AST::TypeClassDeclAST>(def.get())) {
      if (!is_transitive) {
        program_vec.insert(program_vec.begin(),
                                         std::move(def));
      }
    } else if (llvm::isa<AST::TypeClassInstanceAST>(def.get())) {
      if (!is_transitive) {
        program_vec.insert(program_vec.begin(),
                                         std::move(def));
      }
    }
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
