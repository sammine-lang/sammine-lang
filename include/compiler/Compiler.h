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
//! \file Compiler.h
//! \brief Define the Compiler staging
namespace sammine_lang {

using UP = std::unique_ptr<AST::ProgramAST>;
using Path = std::filesystem::path;

class Resolver : public sammine_util::Reportee {

  const Options& options_;
  sammine_util::Location loc_;

  UP program;
public:
  Resolver(const Options& options, const sammine_util::Location& loc) : options_(options), loc_(loc) {}
  void traceLinkableArtifacts(const std::string &name,
                              const std::filesystem::path &src_dir,
                              std::vector<std::string> &extra_object_files);

  Path find_mn(const std::string& name, const Path& src_dir); 

  UP get_program_from_file(const Path& file, const std::string& name);

void inject_definitions(
    std::vector<std::unique_ptr<AST::DefinitionAST>> &DefinitionVec,
    std::vector<std::unique_ptr<AST::DefinitionAST>> &program_vec,
    const std::string &name,
    bool is_transitive);
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
