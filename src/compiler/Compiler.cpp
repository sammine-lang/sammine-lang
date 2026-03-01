//
// Created by Jasmine Tang on 3/27/24.
//

#include "compiler/Compiler.h"
#include "ast/Ast.h"
#include "ast/ASTProperties.h"
#include "codegen/CodegenVisitor.h"
#include "codegen/LLVMRes.h"
#include "codegen/MLIRGen.h"
#include "codegen/MLIRLowering.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
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
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#define DEBUG_TYPE "stages"

// INFO: Declaration
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

  AST::ASTProperties props_;
  sammine_util::Reporter reporter;
  size_t context_radius = 2;
  bool error;
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
  void emit_interface();
  void link();
  void print_timing_table(
      const std::vector<std::pair<const char *, double>> &timings);
  void set_error() { error = true; }
  std::string output_path(const std::string &filename) const {
    if (output_dir.empty())
      return filename;
    return (std::filesystem::path(output_dir) / filename).string();
  }

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
    this->error = true;
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
}

void Compiler::lex() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start lexing stage...\n");
  });
  lexer = std::make_unique<Lexer>(input);
  tokStream = lexer->getTokenStream();
}

void Compiler::parse() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start parsing stage...\n");
  });
  Parser psr = Parser(tokStream, reporter, mod_name);

  auto result = psr.Parse();
  programAST = std::move(result);

  this->error = psr.has_errors();
  reporter.report(*lexer);
  reporter.report(psr);

  for (auto &def : programAST->DefinitionVec)
    if (auto *fd = llvm::dyn_cast<AST::FuncDefAST>(def.get()))
      if (fd->Prototype->functionName.get_name() == "main") {
        has_main = true;
        break;
      }
}

void Compiler::resolve_imports() {
  if (this->error || programAST->imports.empty())
    return;

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start resolve imports stage...\n");
  });

  auto source_dir = std::filesystem::path(this->file_name).parent_path();
  if (source_dir.empty())
    source_dir = ".";

  for (auto &import : programAST->imports) {
    // Look for .mni in CWD, -I paths, source dir, then stdlib dir
    std::filesystem::path mni_path = import.module_name + ".mni";
    if (!std::filesystem::exists(mni_path)) {
      for (auto &ipath : import_paths) {
        auto candidate = std::filesystem::path(ipath) / (import.module_name + ".mni");
        if (std::filesystem::exists(candidate)) {
          mni_path = candidate;
          break;
        }
      }
    }
    if (!std::filesystem::exists(mni_path))
      mni_path = source_dir / (import.module_name + ".mni");
    if (!std::filesystem::exists(mni_path) && !stdlib_dir.empty())
      mni_path = stdlib_dir / (import.module_name + ".mni");
    if (!std::filesystem::exists(mni_path)) {
      reporter.immediate_error(
          fmt::format("Cannot find interface file '{}.mni'. Did you compile "
                      "{}.mn first?",
                      import.module_name, import.module_name),
          import.location);
      this->error = true;
      return;
    }

    // Parse the .mni file — it contains valid sammine extern declarations
    std::string mni_input = sammine_util::get_string_from_file(mni_path.string());
    Lexer mni_lexer(mni_input);
    auto mni_tok_stream = mni_lexer.getTokenStream();
    Parser mni_parser(mni_tok_stream);
    // Set up the module name as its own alias so that qualified type
    // references like str::String resolve correctly in .mni files.
    mni_parser.alias_to_module[import.module_name] = import.module_name;
    auto mni_program = mni_parser.Parse();

    if (mni_parser.has_errors()) {
      reporter.immediate_error(
          fmt::format("Failed to parse interface file '{}'", mni_path.string()),
          import.location);
      this->error = true;
      return;
    }

    // Insert the extern declarations at the front of our DefinitionVec.
    // Override their locations to point to the import statement so that
    // error messages (e.g. name conflicts) reference the user's source.
    for (auto it = mni_program->DefinitionVec.rbegin();
         it != mni_program->DefinitionVec.rend(); ++it) {
      (*it)->set_location(import.location);
      if (auto *ext = llvm::dyn_cast<AST::ExternAST>(it->get())) {
        ext->Prototype->set_location(import.location);
      }
      programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                       std::move(*it));
    }

    // Track the .o file for linking (CWD, -I paths, source dir, then stdlib dir)
    std::filesystem::path obj_path = import.module_name + ".o";
    if (!std::filesystem::exists(obj_path)) {
      for (auto &ipath : import_paths) {
        auto candidate = std::filesystem::path(ipath) / (import.module_name + ".o");
        if (std::filesystem::exists(candidate)) {
          obj_path = candidate;
          break;
        }
      }
    }
    if (!std::filesystem::exists(obj_path))
      obj_path = source_dir / (import.module_name + ".o");
    if (!std::filesystem::exists(obj_path) && !stdlib_dir.empty())
      obj_path = stdlib_dir / (import.module_name + ".o");
    if (std::filesystem::exists(obj_path))
      extra_object_files.push_back(obj_path.string());
  }
}

void Compiler::load_definitions() {
  if (this->error || !has_main)
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

void Compiler::semantics() {
  // INFO: ScopeGeneratorVisitor
  {
    if (this->error) {
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
    this->error = vs.has_errors();
  }

  // INFO: GeneralSemanticsVisitor
  {
    if (this->error)
      return;
    LOG({
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::green),
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
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start bi-direcitonal type checking stage...\n");
  });
  AST::AstBase::set_properties(&props_);
  auto vs = sammine_lang::AST::BiTypeCheckerVisitor(props_);
  programAST->accept_vis(&vs);

  // Inject monomorphized generic enum definitions at the front.
  for (auto it = vs.monomorphized_enum_defs.rbegin();
       it != vs.monomorphized_enum_defs.rend(); ++it) {
    programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                     std::move(*it));
  }

  // Inject monomorphized generic function definitions at the front.
  // Order doesn't matter — codegen forward-declares all functions first.
  for (auto it = vs.monomorphized_defs.rbegin();
       it != vs.monomorphized_defs.rend(); ++it) {
    programAST->DefinitionVec.insert(programAST->DefinitionVec.begin(),
                                     std::move(*it));
  }

  reporter.report(vs);
  this->error = vs.has_errors();
}

void Compiler::linear_check() {
  if (this->error)
    return;
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start linear type checking stage...\n");
  });
  auto lc = sammine_lang::AST::LinearTypeChecker();
  lc.check(programAST.get(), props_);
  reporter.report(lc);
  this->error = lc.has_errors();
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
  if (this->error) {
    LOG({
      fmt::print(stderr,
                 sammine_util::styled(fmt::terminal_color::green),
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
                 sammine_util::styled(fmt::terminal_color::green),
                 "Finished checking. Stopping at codegen stage with compiler's "
                 "--check flag. \n");
    });
    std::exit(0);
  }
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start code-gen stage...\n");
  });

  codegen_mlir();
}

void Compiler::codegen_mlir() {
  mlir::MLIRContext mlirCtx;
  mlirCtx.getOrLoadDialect<mlir::arith::ArithDialect>();
  mlirCtx.getOrLoadDialect<mlir::func::FuncDialect>();
  mlirCtx.getOrLoadDialect<mlir::LLVM::LLVMDialect>();
  mlirCtx.getOrLoadDialect<mlir::scf::SCFDialect>();
  mlirCtx.getOrLoadDialect<mlir::cf::ControlFlowDialect>();

  std::string stem = std::filesystem::path(this->file_name).stem().string();
  std::string moduleName = has_main ? "" : stem;

  auto mlirModule =
      mlirGen(mlirCtx, programAST.get(), moduleName, this->file_name, this->input, props_);
  if (!mlirModule) {
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
               "MLIR generation failed\n");
    this->error = true;
    return;
  }

  if (compiler_options[compiler_option_enum::MLIR_IR] == "true") {
    mlir::OpPrintingFlags flags;
    flags.enableDebugInfo(/*enable=*/true, /*prettyForm=*/true);
    mlirModule->print(llvm::outs(), flags);
    llvm::outs() << "\n";
    std::exit(0);
  }

  auto llvmModule = lowerMLIRToLLVMIR(*mlirModule, *resPtr->Context);
  if (!llvmModule) {
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
               "MLIR lowering to LLVM IR failed\n");
    this->error = true;
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

void Compiler::optimize() {
  if (this->error) {
    std::exit(1);
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

void Compiler::emit_object() {
  if (this->error) {
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

void Compiler::emit_interface() {
  if (this->error || has_main || this->from_string)
    return;

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit interface stage...\n");
  });

  std::string stem = std::filesystem::path(this->file_name).stem().string();
  std::string mni_file = output_path(stem + ".mni");
  std::ofstream out(mni_file);
  if (!out.is_open()) {
    fmt::print(stderr, "Could not open {} for writing\n", mni_file);
    return;
  }

  // Recursively qualify struct type names for .mni emission using QualifiedName
  std::function<std::string(const Type &)> qualify_type =
      [&stem, &qualify_type](const Type &t) -> std::string {
    using sammine_util::QualifiedName;
    switch (t.type_kind) {
    case TypeKind::Struct: {
      auto &st = std::get<StructType>(t.type_data);
      return st.get_name().with_module(stem).mangled();
    }
    case TypeKind::Enum: {
      auto &et = std::get<EnumType>(t.type_data);
      return et.get_name().with_module(stem).mangled();
    }
    case TypeKind::Pointer:
      return (t.is_linear ? "'" : "") + std::string("ptr<") +
             qualify_type(std::get<PointerType>(t.type_data).get_pointee()) +
             ">";
    case TypeKind::Array: {
      auto &arr = std::get<ArrayType>(t.type_data);
      return "[" + qualify_type(arr.get_element()) + ";" +
             std::to_string(arr.get_size()) + "]";
    }
    case TypeKind::Tuple: {
      auto &tup = std::get<TupleType>(t.type_data);
      std::string result = "(";
      for (size_t i = 0; i < tup.size(); i++) {
        result += qualify_type(tup.get_element(i));
        if (i + 1 < tup.size())
          result += ", ";
      }
      result += ")";
      return result;
    }
    default:
      return t.to_string();
    }
  };

  auto emit_proto = [&out, &qualify_type](const std::string &name,
                                          AST::PrototypeAST *proto,
                                          bool is_var_arg) {
    auto funcType = std::get<FunctionType>(proto->get_type().type_data);
    auto paramTypes = funcType.get_params_types();
    auto retType = funcType.get_return_type();

    out << "reuse " << name << "(";
    for (size_t i = 0; i < proto->parameterVectors.size(); i++) {
      auto &param = proto->parameterVectors[i];
      out << param->name;
      out << " : " << qualify_type(paramTypes[i]);
      if (i + 1 < proto->parameterVectors.size() || is_var_arg)
        out << ", ";
    }
    if (is_var_arg)
      out << "...";
    out << ")";
    if (retType.type_kind != TypeKind::Unit)
      out << " -> " << qualify_type(retType);
    out << ";\n";
  };

  for (auto &def : this->programAST->DefinitionVec) {
    if (auto *func_def = llvm::dyn_cast<AST::FuncDefAST>(def.get())) {
      if (!func_def->is_exported)
        continue;
      std::string name = func_def->Prototype->functionName.with_module(stem).mangled();
      emit_proto(name, func_def->Prototype.get(),
                 func_def->Prototype->is_var_arg);
    } else if (auto *extern_def = llvm::dyn_cast<AST::ExternAST>(def.get())) {
      if (!extern_def->is_exposed)
        continue;
      std::string name =
          extern_def->Prototype->functionName.with_module(stem).mangled();
      emit_proto(name, extern_def->Prototype.get(),
                 extern_def->Prototype->is_var_arg);
    } else if (auto *struct_def = llvm::dyn_cast<AST::StructDefAST>(def.get())) {
      if (!struct_def->is_exported)
        continue;
      out << "struct "
          << struct_def->struct_name.with_module(stem).mangled() << " {\n";
      for (size_t i = 0; i < struct_def->struct_members.size(); i++) {
        auto &member = struct_def->struct_members[i];
        out << "  " << member->name;
        if (member->type_expr)
          out << ": " << member->type_expr->to_string();
        out << ",\n";
      }
      out << "};\n";
    } else if (auto *enum_def = llvm::dyn_cast<AST::EnumDefAST>(def.get())) {
      if (!enum_def->is_exported)
        continue;
      out << "type " << enum_def->enum_name.with_module(stem).mangled();
      if (!enum_def->type_params.empty()) {
        out << "<";
        for (size_t i = 0; i < enum_def->type_params.size(); i++) {
          out << enum_def->type_params[i];
          if (i + 1 < enum_def->type_params.size())
            out << ", ";
        }
        out << ">";
      }
      if (enum_def->backing_type_name.has_value())
        out << ": " << *enum_def->backing_type_name;
      out << " = ";
      for (size_t i = 0; i < enum_def->variants.size(); i++) {
        auto &v = enum_def->variants[i];
        out << v.name;
        if (!v.payload_types.empty() || v.discriminant_value.has_value()) {
          out << "(";
          if (v.discriminant_value.has_value()) {
            out << *v.discriminant_value;
          } else {
            for (size_t j = 0; j < v.payload_types.size(); j++) {
              out << v.payload_types[j]->to_string();
              if (j + 1 < v.payload_types.size())
                out << ", ";
            }
          }
          out << ")";
        }
        if (i + 1 < enum_def->variants.size())
          out << " | ";
      }
      out << ";\n";
    }
  }
}

void Compiler::link() {
  if (this->error || this->from_string) {
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

void Compiler::start() {
  if (this->error) return;
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
      {"emit_iface", &Compiler::emit_interface},
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
