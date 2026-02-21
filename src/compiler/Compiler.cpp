//
// Created by Jasmine Tang on 3/27/24.
//

#include "compiler/Compiler.h"
#include "ast/Ast.h"
#include "codegen/CodegenVisitor.h"
#include "codegen/LLVMRes.h"
#include "fmt/color.h"
#include "fmt/core.h"
#include "parser/Parser.h"
#include "semantics/GeneralSemanticsVisitor.h"
#include "semantics/ScopeGeneratorVisitor.h"
#include "typecheck/BiTypeChecker.h"
#include "util/Logging.h"
#include "util/Utilities.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <filesystem>
#include <fstream>

#define DEBUG_TYPE "stages"

// INFO: Declaration
namespace sammine_lang {
class Compiler {
  std::shared_ptr<TokenStream> tokStream;
  std::shared_ptr<AST::ProgramAST> programAST;
  std::map<compiler_option_enum, std::string> compiler_options;
  std::shared_ptr<LLVMRes> resPtr;
  std::string file_name, input;

  const std::set<std::string> pre_func{"printf"};

  sammine_util::Reporter reporter;
  size_t context_radius = 2;
  bool error;

  void lex();
  void parse();
  void semantics();
  void typecheck();
  void dump_ast();
  void codegen();
  void optimize();
  void emit_object();
  void link();
  void print_timing_table(
      const std::vector<std::pair<const char *, double>> &timings);
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

  if (!this->input.empty()) {
    this->file_name = "From string input";
  } else if (!this->file_name.empty()) {
    this->input = sammine_util::get_string_from_file(this->file_name);
  } else {
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_red),
               "[Error during compiler initial phase]\n");
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_red),
               "[Both the file name and the string input is empty]\n");

    std::abort();
  }
  this->resPtr = std::make_shared<LLVMRes>();

  bool dev_mode = compiler_options[compiler_option_enum::DEV] == "true";
  this->reporter =
      sammine_util::Reporter(file_name, input, context_radius, dev_mode);

  // Initialize debug logging from --diagnostics flag
  std::string diagnostic_value =
      compiler_options[compiler_option_enum::DIAGNOSTIC];
  sammine_log::set_enabled_types(diagnostic_value);
}

void Compiler::lex() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_green),
               "Start lexing stage...\n");
  });
  Lexer lxr = Lexer(input);
  reporter.report(lxr);
  tokStream = lxr.getTokenStream();
}

void Compiler::parse() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_green),
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
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::bright_green),
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
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::bright_green),
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
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_green),
               "Start bi-direcitonal type checking stage...\n");
  });
  auto vs = sammine_lang::AST::BiTypeCheckerVisitor(pre_func);
  programAST->accept_vis(&vs);

  // Inject monomorphized generic function definitions at the front
  // so they are codegen'd before call sites that reference them
  for (auto it = vs.monomorphized_defs.rbegin();
       it != vs.monomorphized_defs.rend(); ++it) {
    programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                     std::move(*it));
  }

  reporter.report(vs);
  this->error = vs.has_errors();
}

void Compiler::dump_ast() {
  if (compiler_options[compiler_option_enum::AST_IR] == "true") {
    LOG({
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::bright_green),
                 "Start dumping ast-ir stage...\n");
    });
    AST::ASTPrinter::print(programAST.get());
  }
  if (this->error) {
    LOG({
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::bright_green),
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
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::bright_green),
                 "Finished checking. Stopping at codegen stage with compiler's "
                 "--check flag. \n");
    });
    std::exit(0);
  }
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_green),
               "Start code-gen stage...\n");
  });
  auto vs = sammine_lang::AST::CgVisitor(resPtr);
  programAST->accept_vis(&vs);

  reporter.report(vs);
  this->error = vs.has_errors();
}

void Compiler::optimize() {
  if (this->error) {
    std::exit(1);
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_green),
               "Start optimize stage...\n");
  });
  std::string llvm_ir_mode = compiler_options[compiler_option_enum::LLVM_IR];

  std::string pre_ir;
  if (llvm_ir_mode == "pre" || llvm_ir_mode == "diff") {
    llvm::raw_string_ostream pre_stream(pre_ir);
    resPtr->Module->print(pre_stream, nullptr);
  }

  if (llvm_ir_mode == "pre") {
    llvm::errs() << pre_ir;
  }

  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  llvm::PassBuilder PB(resPtr->target_machine.get());
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::ModulePassManager MPM =
      PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  MPM.run(*resPtr->Module, MAM);

  if (llvm_ir_mode == "post") {
    resPtr->Module->print(llvm::errs(), nullptr);
  } else if (llvm_ir_mode == "diff") {
    std::string post_ir;
    llvm::raw_string_ostream post_stream(post_ir);
    resPtr->Module->print(post_stream, nullptr);

    auto pre_path = std::filesystem::temp_directory_path() / "sammine_pre.ll";
    auto post_path = std::filesystem::temp_directory_path() / "sammine_post.ll";
    {
      std::ofstream pre_file(pre_path);
      pre_file << pre_ir;
    }
    {
      std::ofstream post_file(post_path);
      post_file << post_ir;
    }

    std::string diff_cmd = fmt::format("diff --color -u {} {} 1>&2",
                                       pre_path.string(), post_path.string());
    std::system(diff_cmd.c_str());

    std::filesystem::remove(pre_path);
    std::filesystem::remove(post_path);
  }
}

void Compiler::emit_object() {
  if (this->error) {
    return;
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_green),
               "Start emit object stage...\n");
  });

  std::string stem = std::filesystem::path(this->file_name).stem().string();
  llvm::raw_fd_ostream dest(llvm::raw_fd_ostream(stem + ".o", resPtr->EC));
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
}

void Compiler::link() {
  if (this->error) {
    return;
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::bright_green),
               "Start link stage...\n");
  });

  std::string stem = std::filesystem::path(this->file_name).stem().string();

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
          return;
        else
          sammine_util::abort(
              "Neither clang++ nor g++ is available for final linkage\n");
      }
    }
  }
}
void Compiler::print_timing_table(
    const std::vector<std::pair<const char *, double>> &timings) {
  double total = 0.0;
  for (auto &[name, ms] : timings)
    total += ms;

  fmt::print(stderr, "{:<18} {:>10}  {:>6}\n", "Phase", "Time(ms)", "%");
  fmt::print(stderr, "{:-<18} {:->10}  {:->6}\n", "", "", "");
  for (auto &[name, ms] : timings) {
    double pct = total > 0.0 ? (ms / total) * 100.0 : 0.0;
    fmt::print(stderr, "{:<18} {:>10.2f}  {:>5.1f}%\n", name, ms, pct);
  }
  fmt::print(stderr, "{:-<18} {:->10}  {:->6}\n", "", "", "");
  fmt::print(stderr, "{:<18} {:>10.2f}  {:>5.1f}%\n", "total", total, 100.0);
}

void Compiler::start() {
  struct Stage {
    const char *name;
    std::function<void(Compiler *)> fn;
  };
  std::vector<Stage> stages = {
      {"lex", &Compiler::lex},
      {"parse", &Compiler::parse},
      {"semantics", &Compiler::semantics},
      {"typecheck", &Compiler::typecheck},
      {"dump_ast", &Compiler::dump_ast},
      {"codegen", &Compiler::codegen},
      {"optimize", &Compiler::optimize},
      {"emit_obj", &Compiler::emit_object},
      {"link", &Compiler::link},
  };

  std::string time_level = compiler_options[compiler_option_enum::TIME];
  bool timing = time_level != "false";
  std::vector<std::pair<const char *, double>> timings;

  if (time_level == "coarse")
    llvm::TimePassesIsEnabled = true;

  for (auto &[name, fn] : stages) {
    auto t0 = std::chrono::high_resolution_clock::now();
    fn(this);
    auto t1 = std::chrono::high_resolution_clock::now();
    if (timing) {
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      timings.push_back({name, ms});
    }
  }

  if (timing) {
    if (time_level == "simple") {
      double total = 0.0;
      for (auto &[name, ms] : timings)
        total += ms;
      fmt::print(stderr, "total: {:.2f}ms\n", total);
    } else {
      print_timing_table(timings);
      if (time_level == "coarse")
        llvm::TimerGroup::printAll(llvm::errs());
    }
  }
}

} // namespace sammine_lang
