//
// Created by Jasmine Tang on 3/27/24.
//

#include "compiler/Compiler.h"
#include "ast/Ast.h"
#include "ast/AstBase.h"
#include "codegen/CodegenVisitor.h"
#include "codegen/LLVMRes.h"
#include "fmt/color.h"
#include "fmt/core.h"
#include "lex/Token.h"
#include "parser/Parser.h"
#include "semantics/GeneralSemanticsVisitor.h"
#include "semantics/ScopeGeneratorVisitor.h"
#include "typecheck/BiTypeChecker.h"
#include "util/Logging.h"
#include "util/Utilities.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <system_error>

#define DEBUG_TYPE "stages"

// INFO: Declaration
namespace sammine_lang {
class Compiler {
  std::shared_ptr<TokenStream> tokStream;
  std::shared_ptr<AST::ProgramAST> programAST;
  std::map<compiler_option_enum, std::string> compiler_options;
  std::shared_ptr<LLVMRes> resPtr;
  std::string file_name, input;
  
  const std::set<std::string> pre_func { "printf"};

  sammine_util::Reporter reporter;
  size_t context_radius = 2;
  bool error;

  void lex();
  void parse();
  void semantics();
  void typecheck();
  void dump_ast();
  void codegen();
  void produce_executable();
  void set_error() { error = true; }

public:
  Compiler(std::map<compiler_option_enum, std::string> &compiler_options);
  void start();
};

} // namespace sammine_lang
  //

// INFO: Defn
namespace sammine_lang {
void CompilerRunner::run(
    std::map<compiler_option_enum, std::string> &compiler_options) {
  auto compiler = Compiler(compiler_options);
  compiler.start();
}

Compiler::Compiler(
    std::map<compiler_option_enum, std::string> &compiler_options)
    : compiler_options(compiler_options) {
  this->error = false;
  this->file_name = compiler_options[compiler_option_enum::FILE];
  this->input = compiler_options[compiler_option_enum::STR];

  if (this->input != "") {
    this->file_name = "From string input";
  } else if (this->file_name != "") {
    this->input = sammine_util::get_string_from_file(this->file_name);
  } else {
    fmt::print(stderr, fg(fmt::terminal_color::bright_red),
               "[Error during compiler initial phase]\n");
    fmt::print(stderr, fg(fmt::terminal_color::bright_red),
               "[Both the file name and the string input is empty]\n");

    std::abort();
  }
  this->resPtr = std::make_shared<LLVMRes>();

  *this->resPtr;
  this->reporter = sammine_util::Reporter(file_name, input, context_radius);

  // Initialize debug logging from --diagnostics flag
  std::string diagnostic_value = compiler_options[compiler_option_enum::DIAGNOSTIC];
  sammine_log::set_enabled_types(diagnostic_value);
}

void Compiler::lex() {
  LOG({
    fmt::print(stderr, fg(fmt::terminal_color::bright_green),
               "Start lexing stage...\n");
  });
  Lexer lxr = Lexer(input);
  reporter.report(lxr);
  tokStream = lxr.getTokenStream();
}

void Compiler::parse() {
  LOG({
    fmt::print(stderr, fg(fmt::terminal_color::bright_green),
               "Start parsing stage...\n");
  });
  Parser psr = Parser(tokStream, reporter);

  auto result = psr.Parse();
  programAST = std::move(result);

  this->error = psr.has_errors();
  reporter.report(psr);
}

void Compiler::semantics() {
  // INFO: ScopeGeneratorVisitor
  {
    if (this->error) {
      return;
    }
    LOG({
      fmt::print(stderr, fg(fmt::terminal_color::bright_green),
                 "Start scope checking stage...\n");
    });
    auto vs = sammine_lang::AST::ScopeGeneratorVisitor();

    programAST->accept_vis(&vs);
    reporter.report(vs);
    this->error = vs.has_errors();
  }

  // INFO: GeneralSemanticsVisitor
  {
    if (this->error)
      return;
    LOG({
      fmt::print(stderr, fg(fmt::terminal_color::bright_green),
                 "Start general semantics stage...\n");
    });
    auto vs = sammine_lang::AST::GeneralSemanticsVisitor();

    programAST->accept_vis(&vs);
    reporter.report(vs);
    this->error = vs.has_errors();
  }
}

void Compiler::typecheck() {
  if (this->error) {
    return;
  }
  LOG({
    fmt::print(stderr, fg(fmt::terminal_color::bright_green),
               "Start bi-direcitonal type checking stage...\n");
  });
  auto vs = sammine_lang::AST::BiTypeCheckerVisitor(pre_func);
  programAST->accept_vis(&vs);
  reporter.report(vs);
  this->error = vs.has_errors();
}

void Compiler::dump_ast() {
  if (compiler_options[compiler_option_enum::AST_IR] == "true") {
    LOG({
      fmt::print(stderr, fg(fmt::terminal_color::bright_green),
                 "Start dumping ast-ir stage...\n");
    });
    AST::ASTPrinter::print(programAST.get());
  }
  if (this->error) {
    LOG({
      fmt::print(stderr, fg(fmt::terminal_color::bright_green),
                 "There were errors in previous stages. Aborting now\n");
    });
    std::exit(1);
  }
}
void Compiler::codegen() {
  if (this->error) {
    std::exit(1);
  }
  if (this->compiler_options[compiler_option_enum::CHECK] == "true") {
    LOG({
      fmt::print(stderr, fg(fmt::terminal_color::bright_green),
                 "Finished checking. Stopping at codegen stage with compiler's "
                 "--check flag. \n");
    });
    std::exit(0);
  }
  LOG({
    fmt::print(stderr, fg(fmt::terminal_color::bright_green),
               "Start code-gen stage...\n");
  });
  auto vs = sammine_lang::AST::CgVisitor(resPtr);
  programAST->accept_vis(&vs);

  reporter.report(vs);
  this->error = vs.has_errors();
}

void Compiler::produce_executable() {
  if (this->error) {
    std::exit(1);
  }

  LOG({
    fmt::print(stderr, fg(fmt::terminal_color::bright_green),
               "Start executable/lib stage...\n");
  });
  if (compiler_options[compiler_option_enum::LLVM_IR] == "true") {
    LOG({
      fmt::print(stderr, fg(fmt::terminal_color::bright_green),
                 "Logging pre optimization llvm IR\n");
    });
    resPtr->Module->print(llvm::errs(), nullptr);
  }

  // Output .o and executable in current directory using just the stem
  std::string stem =
      std::filesystem::path(this->file_name).stem().string();
  llvm::raw_fd_ostream dest(
      llvm::raw_fd_ostream(stem + ".o", resPtr->EC));
  if (resPtr->EC) {
    llvm::errs() << "Could not open file: " << resPtr->EC.message();
    return;
  }
  auto FileType = llvm::CodeGenFileType::ObjectFile;

  if (resPtr->target_machine->addPassesToEmitFile(resPtr->pass, dest, nullptr,
                                                  FileType)) {
    llvm::errs() << "TargetMachine can't emit a file of this type";
    return;
  }

  resPtr->pass.run(*resPtr->Module);
  dest.flush();

  auto try_compile_with = [&stem](const std::string &compiler) {
    std::string test_command =
        fmt::format("{} --version", compiler) + " > /dev/null 2>&1";
    int test_result = std::system(test_command.c_str());
    if (test_result != 0)
      return false;
    std::string command =
        fmt::format("{} {}.o -o {}.exe", compiler, stem, stem);
    int result = std::system(command.c_str());
    return result == 0;
  };
  for (auto &def : this->programAST->DefinitionVec) {
    if (auto func_def = dynamic_cast<AST::FuncDefAST *>(def.get())) {
      if (func_def->Prototype->functionName == "main") {
        if (try_compile_with("clang++") || try_compile_with("g++"))
          std::exit(0);
        else
          sammine_util::abort(
              "Neither clang++ nor g++ is available for final linkage\n");
      }
    }
  }
}
void Compiler::start() {
  using CompilerStage = std::function<void(Compiler *)>;
  std::vector<CompilerStage> CompilerStages = {
      {&Compiler::lex},
      {&Compiler::parse},
      {&Compiler::semantics},
      {&Compiler::typecheck},
      {&Compiler::dump_ast},
      {&Compiler::codegen},
      {&Compiler::produce_executable},
  };

  // no error, proceed with current stage
  // error, skip current stage and go next
  // error, compiler-ending stage
  for (auto stage : CompilerStages) {
    stage(this);
  }
}

} // namespace sammine_lang
