
#include "compiler/Compiler.h"
#include "compiler/Options.h"
#include "parser/Parser.h"

namespace sammine_lang {

// Find a .mn file by searching CWD → -I paths → source_dir → stdlib_dir.
std::filesystem::path Resolver::find_mn(const std::string &name,
                                        const std::filesystem::path &src_dir) {
  std::filesystem::path p = name + ".mn";
  if (std::filesystem::exists(p))
    return p;
  for (auto &ipath : options_.import_paths) {
    auto c = std::filesystem::path(ipath) / (name + ".mn");
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
UP Resolver::get_program_from_file(const std::filesystem::path &file_path,
                                   const std::string &name) {

  std::string mn_input = sammine_util::get_string_from_file(file_path.string());
  auto si = std::make_shared<sammine_util::SourceInfo>(
      sammine_util::SourceInfo{file_path.string(), mn_input});
  Lexer mn_lexer(mn_input, si);
  auto mn_tok_stream = mn_lexer.getTokenStream();
  Parser mn_parser(mn_tok_stream);
  mn_parser.alias_to_module[name] = name;
  auto mn_program = mn_parser.Parse();

  if (mn_parser.has_errors()) {
    add_error(loc_,
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
    const std::string &name, const std::filesystem::path &src_dir,
    std::vector<std::string> &extra_object_files) {
  std::vector<std::string> lib_exts =
      options_.lib_format == LibFormat::Static
          ? std::vector<std::string>{".a", ".so"}
          : std::vector<std::string>{".so", ".a"};
  std::filesystem::path obj_path;
  auto find_lib = [&name, &obj_path,
                   &lib_exts](const std::filesystem::path &dir) -> bool {
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
