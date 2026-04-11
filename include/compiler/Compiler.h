//
// Created by Jasmine Tang on 3/27/24.
//

#pragma once
#include "ast/Ast.h"
#include "codegen/LLVMRes.h"
#include "compiler/Options.h"
#include "lex/Lexer.h"
#include "lex/Token.h"
#include "util/Utilities.h"
#include <memory>
#include <optional>
#include <set>
//! \file Compiler.h
//! \brief Define the Compiler staging
namespace sammine_lang {

using UP = std::unique_ptr<AST::ProgramAST>;
using Path = std::filesystem::path;

/// Resolver: owns the `import` resolution stage.
///
/// Mirrors the Parser stage — construct once, call Resolve(), then flush
/// queued diagnostics via `reporter.report(resolver)`. Immediate, unlocated
/// errors (e.g. "cannot find module 'foo.mn'") are routed through the
/// optional Reporter reference, matching Parser's pattern.
class Resolver : public sammine_util::Reportee {
  const Options &options_;
  std::optional<std::reference_wrapper<sammine_util::Reporter>> reporter;

  /// Tracks canonical module names already imported to break cycles and
  /// deduplicate transitive chains.
  std::set<std::string> imported_modules;

  /// Recursively import a module's definitions into `programAST`.
  ///   - Direct imports (is_transitive=false): pull exported defs + all
  ///     generic defs + link artifact.
  ///   - Transitive imports (is_transitive=true): only pull generic function
  ///     defs and type defs needed by the monomorphizer; no link artifact.
  bool import_module(const std::string &name, const Path &src_dir,
                     const sammine_util::Location &loc, bool is_transitive,
                     AST::ProgramAST &programAST,
                     std::vector<std::string> &extra_object_files);

  /// Locate a linkable artifact (.a/.so) for `name` and append it to
  /// `extra_object_files`. Search order mirrors find_mn.
  void traceLinkableArtifacts(const std::string &name, const Path &src_dir,
                              std::vector<std::string> &extra_object_files);

  /// Find a .mn file by searching CWD → -I paths → source_dir → stdlib_dir.
  Path find_mn(const std::string &name, const Path &src_dir);

  /// Lex + parse a .mn file. On failure, queues an error at `loc` against
  /// this Reportee and returns nullptr.
  UP get_program_from_file(const Path &file, const std::string &name,
                           const sammine_util::Location &loc);

  /// Filter and inject definitions from an imported module into the main
  /// program's DefinitionVec, respecting is_exported / is_generic rules.
  void inject_definitions(
      std::vector<std::unique_ptr<AST::DefinitionAST>> &DefinitionVec,
      std::vector<std::unique_ptr<AST::DefinitionAST>> &program_vec,
      const std::string &name, bool is_transitive);

public:
  Resolver(const Options &options,
           std::optional<std::reference_wrapper<sammine_util::Reporter>>
               reporter_ = std::nullopt)
      : options_(options), reporter(reporter_) {}

  /// Entry point: resolve every `import` in `programAST` (recursively),
  /// mutating `programAST->DefinitionVec` and appending link artifacts to
  /// `extra_object_files`. Stops early on the first fatal error.
  void Resolve(AST::ProgramAST &programAST, const Path &source_dir,
               std::vector<std::string> &extra_object_files);
};

class Compiler {
  Options options_;
  std::shared_ptr<TokenStream> tokStream;
  std::unique_ptr<Lexer> lexer;
  std::shared_ptr<AST::ProgramAST> programAST;
  std::shared_ptr<LLVMRes> resPtr;
  std::string mod_name;
  std::vector<std::string> extra_object_files;
  std::vector<std::string> extra_link_objs_;

  AST::ASTProperties props_;
  enum class State { Running, Finished, Error };

  sammine_util::Reporter reporter;
  sammine_util::Reportee reportee;
  int64_t context_radius = 2;
  State state_ = State::Running;
  bool has_main = false;
  int jit_exit_code_ = 0;

  void lex();
  void parse();
  void resolve_imports();
  void load_definitions();
  void semantics();
  void typecheck();
  void linear_check();
  void dump_ast();
  void codegen();
  void codegen_mlir();
  void optimize();
  void emit_object();
  void emit_library();
  void emit_archive_impl();
  void emit_shared_impl();
  void link();
  void jit_execute();

  void print_timing_table(
      const std::vector<std::pair<const char *, double>> &timings);
  bool has_error() const { return state_ == State::Error; }
  bool should_stop() const { return state_ != State::Running; }
  void set_error() { state_ = State::Error; }
  std::string output_path(const std::string &filename) const {
    if (options_.output_dir.empty())
      return filename;
    return (options_.output_dir / filename).string();
  }

  void traceLinkableArtifacts(const std::string &name,
                              const std::filesystem::path &src_dir);

public:
  Compiler(const Options &options);
  int start();
};

class CompilerRunner {
public:
  static int run(const Options &options);
};

} // namespace sammine_lang
