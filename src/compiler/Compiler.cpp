//
// Created by Jasmine Tang on 3/27/24.
//

#include "compiler/Compiler.h"
#include "ast/Ast.h"
#include "ast/ASTProperties.h"
#include "codegen/LLVMRes.h"
#include "codegen/MLIRGen.h"
#include "codegen/MLIRLowering.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Arith/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Arith/Transforms/BufferDeallocationOpInterfaceImpl.h"
#include "mlir/Dialect/Bufferization/Transforms/FuncBufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/ControlFlow/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/ControlFlow/Transforms/BufferDeallocationOpInterfaceImpl.h"
#include "mlir/Dialect/Linalg/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/SCF/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/SCF/Transforms/BufferDeallocationOpInterfaceImpl.h"
#include "mlir/Dialect/Tensor/Transforms/BufferizableOpInterfaceImpl.h"
#include "fmt/color.h"
#include "fmt/core.h"
#include "lex/Lexer.h"
#include "parser/Parser.h"
#include "semantics/GeneralSemanticsVisitor.h"
#include "semantics/ScopeGeneratorVisitor.h"
#include "typecheck/BiTypeChecker.h"
#include "typecheck/LinearTypeChecker.h"
#include "util/Logging.h"
#include "util/QualifiedName.h"
#include "util/Utilities.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#define DEBUG_TYPE "stages"

// Compiler: orchestrates the full compilation pipeline.
// Each stage method short-circuits if has_error() is true.
namespace sammine_lang {
class Compiler {
  std::shared_ptr<TokenStream> tokStream;
  std::unique_ptr<Lexer> lexer;
  std::shared_ptr<AST::ProgramAST> programAST;
  std::map<compiler_option_enum, std::string> compiler_options;
  std::shared_ptr<LLVMRes> resPtr;
  std::string file_name, input;
  std::string mod_name;
  std::vector<std::string> extra_object_files;
  std::filesystem::path stdlib_dir;
  std::string output_dir;
  std::vector<std::string> import_paths;
  LibFormat lib_format_ = LibFormat::None;
  std::vector<std::string> extra_link_objs_;

  AST::ASTProperties props_;
  enum class State { Running, Finished, Error };

  sammine_util::Reporter reporter;
  int64_t context_radius = 2;
  State state_ = State::Running;
  bool has_main = false;
  bool from_string = false;

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

  static LibFormat parse_lib_format(const std::string &s) {
    if (s == "static") return LibFormat::Static;
    if (s == "shared") return LibFormat::Shared;
    return LibFormat::None;
  }
  void print_timing_table(
      const std::vector<std::pair<const char *, double>> &timings);
  bool has_error() const { return state_ == State::Error; }
  bool should_stop() const { return state_ != State::Running; }
  void set_error() { state_ = State::Error; }
  std::string output_path(const std::string &filename) const {
    if (output_dir.empty())
      return filename;
    return (std::filesystem::path(output_dir) / filename).string();
  }

public:
  Compiler(std::map<compiler_option_enum, std::string> &compiler_options);
  int start();
};

} // namespace sammine_lang
  //

namespace sammine_lang {
int CompilerRunner::run(
    std::map<compiler_option_enum, std::string> &compiler_options) {
  auto compiler = Compiler(compiler_options);
  return compiler.start();
}

Compiler::Compiler(
    std::map<compiler_option_enum, std::string> &compiler_options)
    : compiler_options(compiler_options) {
  this->state_ = State::Running;
  this->file_name = compiler_options[compiler_option_enum::FILE];
  this->input = compiler_options[compiler_option_enum::STR];

 this->mod_name = "sammine";
  if (!this->input.empty()) {
    this->file_name = "-s|--str";
    this->from_string = true;
  } else if (!this->file_name.empty()) {
    this->input = sammine_util::get_string_from_file(this->file_name);
  } else {
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
               "[Error during compiler initial phase]\n");
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
               "[Both the file name and the string input is empty]\n");
    set_error();
    return;
  }
  this->resPtr = std::make_shared<LLVMRes>();

  // Initialize debug logging from --diagnostics flag
  std::string diagnostic_value =
      compiler_options[compiler_option_enum::DIAGNOSTIC];
  sammine_log::set_enabled_types(diagnostic_value);

  bool dev_mode = sammine_log::is_type_in_list("dev", diagnostic_value);
  this->reporter =
      sammine_util::Reporter(file_name, input, context_radius, dev_mode);

  // Initialize output directory
  output_dir = compiler_options[compiler_option_enum::OUTPUT_DIR];
  if (!output_dir.empty())
    std::filesystem::create_directories(output_dir);

  // Parse semicolon-joined import paths
  {
    std::string paths_str = compiler_options[compiler_option_enum::IMPORT_PATHS];
    if (!paths_str.empty()) {
      std::istringstream ss(paths_str);
      std::string path;
      while (std::getline(ss, path, ';'))
        if (!path.empty())
          import_paths.push_back(path);
    }
  }

  // Compute stdlib directory relative to the binary location
  std::string argv0 = compiler_options[compiler_option_enum::ARGV0];
  if (!argv0.empty()) {
    std::error_code ec;
    auto bin_path = std::filesystem::canonical(argv0, ec);
    if (!ec)
      stdlib_dir = bin_path.parent_path().parent_path() / "lib" / "sammine";
  }

  lib_format_ = parse_lib_format(compiler_options[compiler_option_enum::LIB_FORMAT]);
}

// Stage 1: Tokenize source into a TokenStream.
void Compiler::lex() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start lexing stage...\n");
  });
  lexer = std::make_unique<Lexer>(input);
  tokStream = lexer->getTokenStream();
}

// Stage 2: Parse tokens into AST. Also detects whether `main` exists (→ executable vs library).
void Compiler::parse() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start parsing stage...\n");
  });
  Parser psr = Parser(tokStream, reporter, mod_name);

  auto result = psr.Parse();
  programAST = std::move(result);

  if (psr.has_errors()) set_error();
  reporter.report(*lexer);
  reporter.report(psr);

  for (auto &def : programAST->DefinitionVec)
    if (auto *fd = llvm::dyn_cast<AST::FuncDefAST>(def.get()))
      if (fd->Prototype->functionName.get_name() == "main") {
        has_main = true;
        break;
      }
}

// Stage 3: Resolve `import` declarations by parsing .mn files on the fly.
// Recursively handles transitive imports. Deduplicates via canonical module name.
// Exported defs become ExternASTs; generic defs are inlined for monomorphization.
void Compiler::resolve_imports() {
  if (has_error() || programAST->imports.empty())
    return;

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start resolve imports stage...\n");
  });

  auto source_dir = std::filesystem::path(this->file_name).parent_path();
  if (source_dir.empty())
    source_dir = ".";

  // Track already-imported modules to avoid duplicates in transitive chains.
  std::set<std::string> imported_modules;

  // Find a .mn file by searching CWD → -I paths → source_dir → stdlib_dir.
  auto find_mn = [&](const std::string &name,
                     const std::filesystem::path &src_dir)
      -> std::filesystem::path {
    std::filesystem::path p = name + ".mn";
    if (std::filesystem::exists(p))
      return p;
    for (auto &ipath : import_paths) {
      auto c = std::filesystem::path(ipath) / (name + ".mn");
      if (std::filesystem::exists(c))
        return c;
    }
    p = src_dir / (name + ".mn");
    if (std::filesystem::exists(p))
      return p;
    if (!stdlib_dir.empty()) {
      p = stdlib_dir / (name + ".mn");
      if (std::filesystem::exists(p))
        return p;
    }
    return {};
  };

  // Recursively import a module's definitions into programAST.
  // - For direct imports: pull exported defs + all generic defs + link artifact.
  // - For transitive imports (is_transitive=true): only pull generic function
  //   defs and type defs needed by the monomorphizer — no link artifact.
  std::function<bool(const std::string &, const std::filesystem::path &,
                     const sammine_util::Location &, bool)>
      import_module = [&](const std::string &name,
                          const std::filesystem::path &src_dir,
                          const sammine_util::Location &loc,
                          bool is_transitive) -> bool {
    if (imported_modules.count(name))
      return true;
    imported_modules.insert(name);

    auto mn_path = find_mn(name, src_dir);
    if (mn_path.empty()) {
      if (!is_transitive) {
        reporter.immediate_error(
            fmt::format("Cannot find module '{}.mn'", name), loc);
        set_error();
      }
      return false;
    }

    // Parse the .mn file with SourceInfo for cross-file error locations.
    std::string mn_input = sammine_util::get_string_from_file(mn_path.string());
    auto si = std::make_shared<sammine_util::SourceInfo>(
        sammine_util::SourceInfo{mn_path.string(), mn_input});
    Lexer mn_lexer(mn_input, si);
    auto mn_tok_stream = mn_lexer.getTokenStream();
    Parser mn_parser(mn_tok_stream);
    mn_parser.alias_to_module[name] = name;
    auto mn_program = mn_parser.Parse();

    if (mn_parser.has_errors()) {
      if (!is_transitive) {
        reporter.immediate_error(
            fmt::format("Failed to parse module '{}'", mn_path.string()), loc);
        set_error();
      }
      return false;
    }

    // Recursively import this module's own dependencies (transitive).
    auto mod_src_dir = mn_path.parent_path();
    for (auto &sub_import : mn_program->imports)
      import_module(sub_import.module_name, mod_src_dir,
                    sub_import.location, true);

    // Filter definitions and inject into programAST.
    for (auto it = mn_program->DefinitionVec.rbegin();
         it != mn_program->DefinitionVec.rend(); ++it) {
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
          programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                           std::move(def));
        } else {
          // Non-generic exported → extern
          auto ext = std::make_unique<AST::ExternAST>(
              std::move(fd->Prototype));
          ext->is_exposed = fd->is_exported;
          programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                           std::move(ext));
        }
      } else if (auto *ext = llvm::dyn_cast<AST::ExternAST>(def.get())) {
        if (is_transitive || !ext->is_exposed)
          continue;
        ext->Prototype->functionName =
            ext->Prototype->functionName.with_module(name);
        programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                         std::move(def));
      } else if (auto *sd = llvm::dyn_cast<AST::StructDefAST>(def.get())) {
        if (!sd->is_exported)
          continue;
        sd->struct_name = sd->struct_name.with_module(name);
        programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                         std::move(def));
      } else if (auto *ed = llvm::dyn_cast<AST::EnumDefAST>(def.get())) {
        if (!ed->is_exported)
          continue;
        ed->enum_name = ed->enum_name.with_module(name);
        programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                         std::move(def));
      } else if (auto *ta = llvm::dyn_cast<AST::TypeAliasDefAST>(def.get())) {
        if (!ta->is_exported)
          continue;
        programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                         std::move(def));
      } else if (llvm::isa<AST::TypeClassDeclAST>(def.get())) {
        if (!is_transitive) {
          programAST->DefinitionVec.insert(
              programAST->DefinitionVec.begin(), std::move(def));
        }
      } else if (llvm::isa<AST::TypeClassInstanceAST>(def.get())) {
        if (!is_transitive) {
          programAST->DefinitionVec.insert(
              programAST->DefinitionVec.begin(), std::move(def));
        }
      }
    }

    // Track linkable artifact (only for direct imports).
    if (!is_transitive) {
      std::vector<std::string> lib_exts =
          lib_format_ == LibFormat::Static
              ? std::vector<std::string>{".a", ".so"}
              : std::vector<std::string>{".so", ".a"};
      std::filesystem::path obj_path;
      auto find_lib = [&](const std::filesystem::path &dir) -> bool {
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
        for (auto &ipath : import_paths)
          if (find_lib(ipath))
            break;
      }
      if (obj_path.empty())
        find_lib(src_dir);
      if (obj_path.empty() && !stdlib_dir.empty())
        find_lib(stdlib_dir);
      if (!obj_path.empty())
        extra_object_files.push_back(obj_path.string());
    }

    return true;
  };

  for (auto &import : programAST->imports) {
    import_module(import.module_name, source_dir, import.location, false);
    if (has_error())
      return;
  }
}

// Stage 4: Load stdlib/definitions.mn (builtin type defs). Executables only.
void Compiler::load_definitions() {
  if (has_error() || !has_main)
    return;

  // Find definitions.mn in stdlib_dir
  std::filesystem::path def_path;
  if (!stdlib_dir.empty())
    def_path = stdlib_dir / "definitions.mn";

  if (def_path.empty() || !std::filesystem::exists(def_path))
    return;

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start load definitions stage...\n");
  });

  std::string def_input = sammine_util::get_string_from_file(def_path.string());
  Lexer def_lexer(def_input);
  auto def_tok_stream = def_lexer.getTokenStream();
  Parser def_parser(def_tok_stream);
  auto def_program = def_parser.Parse();

  if (def_parser.has_errors()) {
    fmt::print(stderr, "Warning: failed to parse {}\n", def_path.string());
    return;
  }

  // Prepend definitions to user's program
  for (auto it = def_program->DefinitionVec.rbegin();
       it != def_program->DefinitionVec.rend(); ++it) {
    programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                     std::move(*it));
  }
}

// Stage 5: Two semantic sub-passes:
// 1) ScopeGenerator: builds scopes, resolves names, qualifies enum variants
// 2) GeneralSemantics: general validation checks
void Compiler::semantics() {
  {
    if (has_error()) {
      return;
    }
    LOG({
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::green),
                 "Start scope checking stage...\n");
    });
    auto vs = sammine_lang::AST::ScopeGeneratorVisitor();

    programAST->accept_vis(&vs);
    reporter.report(vs);
    if (vs.has_errors()) set_error();
  }

  // INFO: GeneralSemanticsVisitor
  {
    if (has_error())
      return;
    LOG({
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::green),
                 "Start general semantics stage...\n");
    });
    auto vs = sammine_lang::AST::GeneralSemanticsVisitor();

    programAST->accept_vis(&vs);
    reporter.report(vs);
    if (vs.has_errors()) set_error();
  }
}

// Stage 6: Bidirectional type checking + monomorphization.
// Infers types bottom-up, checks assignments/calls, instantiates generics.
// Monomorphized defs are injected at the front of DefinitionVec for codegen.
void Compiler::typecheck() {
  if (has_error()) {
    return;
  }
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start bi-direcitonal type checking stage...\n");
  });
  AST::AstBase::set_properties(&props_);
  auto vs = sammine_lang::AST::BiTypeCheckerVisitor(props_);
  programAST->accept_vis(&vs);

  // Inject monomorphized generic struct definitions at the front.
  for (auto it = vs.monomorphizer.monomorphized_struct_defs.rbegin();
       it != vs.monomorphizer.monomorphized_struct_defs.rend(); ++it) {
    programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                     std::move(*it));
  }

  // Inject monomorphized generic enum definitions at the front.
  for (auto it = vs.monomorphizer.monomorphized_enum_defs.rbegin();
       it != vs.monomorphizer.monomorphized_enum_defs.rend(); ++it) {
    programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                     std::move(*it));
  }

  // Inject monomorphized generic function definitions at the front.
  // Order doesn't matter — codegen forward-declares all functions first.
  for (auto it = vs.monomorphizer.monomorphized_defs.rbegin();
       it != vs.monomorphizer.monomorphized_defs.rend(); ++it) {
    programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                     std::move(*it));
  }

  reporter.report(vs);
  if (vs.has_errors()) set_error();
}

// Stage 7: Linear type checking — ensures 'ptr<T> values are consumed exactly once.
void Compiler::linear_check() {
  if (has_error())
    return;
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start linear type checking stage...\n");
  });
  auto lc = sammine_lang::AST::LinearTypeChecker();
  lc.check(programAST.get(), props_);
  reporter.report(lc);
  if (lc.has_errors()) set_error();
}

void Compiler::dump_ast() {
  if (compiler_options[compiler_option_enum::AST_IR] == "true") {
    LOG({
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::green),
                 "Start dumping ast-ir stage...\n");
    });
    AST::ASTPrinter::print(programAST.get(), props_);
  }
  if (has_error()) {
    LOG({
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::green),
                 "There were errors in previous stages. Aborting now\n");
    });
    return;
  }
}
void Compiler::codegen() {
  if (has_error()) {
    return;
  }
  if (this->compiler_options[compiler_option_enum::CHECK] == "true") {
    LOG({
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::green),
                 "Finished checking. Stopping at codegen stage with compiler's "
                 "--check flag. \n");
    });
    state_ = State::Finished;
    return;
  }
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start code-gen stage...\n");
  });

  codegen_mlir();
}

// MLIR codegen: registers dialects (Arith, Func, LLVM, SCF, CF), generates MLIR
// from AST via mlirGen(), then lowers MLIR→LLVM IR. Transfers data layout/triple
// from the template LLVMRes module to the newly lowered module.
void Compiler::codegen_mlir() {
  mlir::MLIRContext mlirCtx;
  mlirCtx.getOrLoadDialect<mlir::arith::ArithDialect>();
  mlirCtx.getOrLoadDialect<mlir::func::FuncDialect>();
  mlirCtx.getOrLoadDialect<mlir::LLVM::LLVMDialect>();
  mlirCtx.getOrLoadDialect<mlir::scf::SCFDialect>();
  mlirCtx.getOrLoadDialect<mlir::cf::ControlFlowDialect>();
  mlirCtx.getOrLoadDialect<mlir::linalg::LinalgDialect>();
  mlirCtx.getOrLoadDialect<mlir::tensor::TensorDialect>();
  mlirCtx.getOrLoadDialect<mlir::bufferization::BufferizationDialect>();
  mlirCtx.getOrLoadDialect<mlir::affine::AffineDialect>();
  mlirCtx.getOrLoadDialect<mlir::memref::MemRefDialect>();

  // Register bufferization interface extensions for each dialect so
  // one-shot-bufferize knows how to bufferize their ops.
  mlir::DialectRegistry registry;
  mlir::arith::registerBufferizableOpInterfaceExternalModels(registry);
  mlir::arith::registerBufferDeallocationOpInterfaceExternalModels(registry);
  mlir::bufferization::func_ext::registerBufferizableOpInterfaceExternalModels(
      registry);
  mlir::cf::registerBufferizableOpInterfaceExternalModels(registry);
  mlir::cf::registerBufferDeallocationOpInterfaceExternalModels(registry);
  mlir::linalg::registerBufferizableOpInterfaceExternalModels(registry);
  mlir::scf::registerBufferizableOpInterfaceExternalModels(registry);
  mlir::scf::registerBufferDeallocationOpInterfaceExternalModels(registry);
  mlir::tensor::registerBufferizableOpInterfaceExternalModels(registry);
  mlirCtx.appendDialectRegistry(registry);

  std::string stem = std::filesystem::path(this->file_name).stem().string();
  std::string moduleName = has_main ? "" : stem;

  auto mlirModule =
      mlirGen(mlirCtx, programAST.get(), moduleName, this->file_name, this->input, props_);
  if (!mlirModule) {
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
               "MLIR generation failed\n");
    set_error();
    return;
  }

  if (compiler_options[compiler_option_enum::MLIR_IR] == "true") {
    mlir::OpPrintingFlags flags;
    flags.enableDebugInfo(/*enable=*/true, /*prettyForm=*/true);
    mlirModule->print(llvm::outs(), flags);
    llvm::outs() << "\n";
    state_ = State::Finished;
    return;
  }

  auto llvmModule = lowerMLIRToLLVMIR(*mlirModule, *resPtr->Context);
  if (!llvmModule) {
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
               "MLIR lowering to LLVM IR failed\n");
    set_error();
    return;
  }

  // Create IFuncs for exported C externs in library modules.
  // Externs are declared with their C name (@printf), but importers look for
  // the mangled name (@module$printf). IFuncs bridge that gap.
  if (!moduleName.empty()) {
    for (auto &def : programAST->DefinitionVec) {
      auto *ext = llvm::dyn_cast<AST::ExternAST>(def.get());
      if (!ext) continue;
      std::string cName = ext->Prototype->functionName.mangled();
      std::string mangled = ext->Prototype->functionName.with_module(moduleName).mangled();
      auto *fn = llvmModule->getFunction(cName);
      if (!fn || llvmModule->getNamedValue(mangled)) continue;

      auto *ptrTy = llvm::PointerType::get(*resPtr->Context, 0);
      auto *resolverTy = llvm::FunctionType::get(ptrTy, false);
      auto *resolver = llvm::Function::Create(
          resolverTy, llvm::Function::InternalLinkage,
          "resolve_" + mangled, llvmModule.get());
      auto *entry =
          llvm::BasicBlock::Create(*resPtr->Context, "entry", resolver);
      llvm::IRBuilder<> irBuilder(entry);
      irBuilder.CreateRet(fn);

      llvm::GlobalIFunc::create(fn->getFunctionType(), 0,
                                llvm::GlobalValue::ExternalLinkage,
                                mangled, resolver, llvmModule.get());
    }
  }

  llvmModule->setDataLayout(resPtr->Module->getDataLayout());
  llvmModule->setTargetTriple(resPtr->Module->getTargetTriple());
  resPtr->Module = std::move(llvmModule);
}

// Stage 9: Run LLVM O2 optimization pipeline. Supports --llvm-ir pre/post/diff modes.
void Compiler::optimize() {
  if (has_error()) {
    return;
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
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

// Stage 10: Emit .o object file via LLVM TargetMachine.
void Compiler::emit_object() {
  if (has_error()) {
    return;
  }

  if (this->from_string) {
    reporter.immediate_diag(
        "Skipping object file and executable emission: input was provided "
        "via --str. Use -f to compile from a file.");
    return;
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit object stage...\n");
  });

  std::string stem = std::filesystem::path(this->file_name).stem().string();
  std::string obj_file = output_path(stem + ".o");
  llvm::raw_fd_ostream dest(llvm::raw_fd_ostream(obj_file, resPtr->EC));
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


// Create a static archive (.a) by collecting this module's .o + transitive deps.
void Compiler::emit_archive_impl() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit archive stage...\n");
  });

  std::string stem = std::filesystem::path(this->file_name).stem().string();
  std::string obj_file = output_path(stem + ".o");
  std::string archive_file = output_path(stem + ".a");

  // Collect all .o files into a temp directory, then create the archive.
  // For .a deps, extract their members first.
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
      std::system(cmd.c_str());
    } else {
      std::filesystem::copy_file(
          dep, tmp_dir / std::filesystem::path(dep).filename(),
          std::filesystem::copy_options::overwrite_existing);
    }
  };

  for (auto &dep : extra_object_files)
    copy_dep(dep);
  for (auto &dep : extra_link_objs_)
    copy_dep(dep);

  // Create the archive from all collected .o files
  auto abs_archive = std::filesystem::absolute(archive_file).string();
  std::string cmd =
      fmt::format("cd {} && ar rcs {} *.o", tmp_dir.string(), abs_archive);
  int result = std::system(cmd.c_str());
  if (result != 0)
    fmt::print(stderr, "Failed to create archive {}\n", archive_file);

  // Clean up temp directory
  std::filesystem::remove_all(tmp_dir);
}

// Create a shared library (.so) via clang++ -shared.
void Compiler::emit_shared_impl() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit shared library stage...\n");
  });

  std::string stem = std::filesystem::path(this->file_name).stem().string();
  std::string obj_file = output_path(stem + ".o");
  std::string lib_file = output_path(stem + ".so");

  std::string extra;
  for (auto &obj : extra_link_objs_)
    extra += " " + obj;
  std::string command =
      fmt::format("clang++ -shared -undefined dynamic_lookup -o {} {}{}",
                  lib_file, obj_file, extra);
  int result = std::system(command.c_str());
  if (result != 0) {
    fmt::print(stderr, "Failed to create shared library {}\n", lib_file);
  }
}

void Compiler::emit_library() {
  if (has_error() || has_main || this->from_string)
    return;

  // Default to shared library when no --lib flag is given
  if (lib_format_ == LibFormat::None)
    lib_format_ = LibFormat::Shared;

  // Auto-detect <stem>_runtime.o for library builds.
  {
    std::string stem = std::filesystem::path(this->file_name).stem().string();
    std::string runtime_obj = stem + "_runtime.o";
    bool found = false;
    if (!output_dir.empty()) {
      auto candidate = std::filesystem::path(output_dir) / runtime_obj;
      if (std::filesystem::exists(candidate)) {
        extra_link_objs_.push_back(candidate.string());
        found = true;
      }
    }
    if (!found) {
      auto source_dir = std::filesystem::path(file_name).parent_path();
      if (source_dir.empty()) source_dir = ".";
      auto candidate = source_dir / runtime_obj;
      if (std::filesystem::exists(candidate))
        extra_link_objs_.push_back(candidate.string());
    }
  }

  switch (lib_format_) {
  case LibFormat::Static:
    emit_archive_impl();
    break;
  case LibFormat::Shared:
    emit_shared_impl();
    break;
  case LibFormat::None:
    break;
  }

  // Clean up intermediate .o — only the .a/.so is the deliverable
  std::string stem = std::filesystem::path(this->file_name).stem().string();
  std::string obj_file = output_path(stem + ".o");
  std::filesystem::remove(obj_file);
}

// Stage 12: Link .o files into executable via clang++ (fallback: g++). Executables only.
void Compiler::link() {
  if (has_error() || this->from_string) {
    return;
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start link stage...\n");
  });

  std::string stem = std::filesystem::path(this->file_name).stem().string();
  std::string obj_file = output_path(stem + ".o");
  std::string exe_file = output_path(stem + ".exe");

  std::string obj_list = obj_file;
  for (auto &obj : extra_object_files)
    obj_list += " " + obj;

  auto try_compile_with = [&obj_list, &exe_file](const std::string &compiler) {
    std::string test_command =
        fmt::format("{} --version", compiler) + " > /dev/null 2>&1";
    int test_result = std::system(test_command.c_str());
    if (test_result != 0)
      return false;
    std::string command =
        fmt::format("{} {} -o {}", compiler, obj_list, exe_file);
    int result = std::system(command.c_str());
    return result == 0;
  };
  if (!has_main)
    return;
  if (!try_compile_with("clang++") && !try_compile_with("g++"))
    sammine_util::abort(
        "Neither clang++ nor g++ is available for final linkage\n");
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

// Main entry point: runs all pipeline stages sequentially with optional timing.
// Returns 0 on success, 1 on error.
int Compiler::start() {
  if (has_error()) return 1;
  struct Stage {
    const char *name;
    std::function<void(Compiler *)> fn;
  };
  std::vector<Stage> stages = {
      {"lex", &Compiler::lex},
      {"parse", &Compiler::parse},
      {"imports", &Compiler::resolve_imports},
      {"definitions", &Compiler::load_definitions},
      {"semantics", &Compiler::semantics},
      {"typecheck", &Compiler::typecheck},
      {"linear_check", &Compiler::linear_check},
      {"dump_ast", &Compiler::dump_ast},
      {"codegen", &Compiler::codegen},
      {"optimize", &Compiler::optimize},
      {"emit_obj", &Compiler::emit_object},
      {"emit_library", &Compiler::emit_library},
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
    if (should_stop())
      break;
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

  return has_error() ? 1 : 0;
}

} // namespace sammine_lang
