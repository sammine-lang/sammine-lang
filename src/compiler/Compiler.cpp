//
// Created by Jasmine Tang on 3/27/24.
//

#include "compiler/Compiler.h"
#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "codegen/LLVMRes.h"
#include "codegen/MLIRGen.h"
#include "codegen/MLIRLowering.h"
#include "compiler/Options.h"
#include "fmt/color.h"
#include "fmt/core.h"
#include "lex/Lexer.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ComplexToLLVM/ComplexToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/ConvertToLLVM/ToLLVMPass.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/GPUCommon/GPUToLLVM.h"
#include "mlir/Conversion/IndexToLLVM/IndexToLLVM.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/NVVMToLLVM/NVVMToLLVM.h"
#include "mlir/Conversion/UBToLLVM/UBToLLVM.h"
#include "mlir/Conversion/VectorToLLVM/ConvertVectorToLLVM.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Transforms/BufferDeallocationOpInterfaceImpl.h"
#include "mlir/Dialect/Arith/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Bufferization/Transforms/FuncBufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/ControlFlow/Transforms/BufferDeallocationOpInterfaceImpl.h"
#include "mlir/Dialect/ControlFlow/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/MemRef/Transforms/AllocationOpInterfaceImpl.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/SCF/Transforms/BufferDeallocationOpInterfaceImpl.h"
#include "mlir/Dialect/SCF/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tensor/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/Target/LLVM/NVVM/Target.h"
#include "mlir/Target/LLVMIR/Dialect/GPU/GPUToLLVMIRTranslation.h"
#include "parser/Parser.h"
#include "semantics/GeneralSemanticsVisitor.h"
#include "semantics/ScopeGeneratorVisitor.h"
#include "typecheck/BiTypeChecker.h"
#include "typecheck/LinearTypeChecker.h"
#include "util/Logging.h"
#include "util/QualifiedName.h"
#include "util/Tracy.h"
#include "util/Utilities.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
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

} // namespace sammine_lang
  //

namespace sammine_lang {
int CompilerRunner::run(const Options &options) {

  auto compiler = Compiler(options);
  return compiler.start();
}

Compiler::Compiler(const Options &options) : options_(options) {
  this->state_ = State::Running;

  this->mod_name = "sammine";
  this->resPtr = std::make_shared<LLVMRes>();

  // Initialize debug logging from --diagnostics flag
  sammine_log::set_enabled_types(options_.diagnostics);
  bool dev_mode = sammine_log::is_type_in_list("dev", options_.diagnostics);
  this->reporter = sammine_util::Reporter(options_.file_arg, options_.str_arg,
                                          context_radius, dev_mode);
}

// Stage 1: Tokenize source into a TokenStream.
void Compiler::lex() {
  SAMMINE_ZONE_NAMED("lex");
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start lexing stage...\n");
  });
  lexer = std::make_unique<Lexer>(options_.str_arg);
  tokStream = lexer->getTokenStream();
}

// Stage 2: Parse tokens into AST. Also detects whether `main` exists (→
// executable vs library).
void Compiler::parse() {
  SAMMINE_ZONE_NAMED("parse");
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start parsing stage...\n");
  });
  Parser psr = Parser(tokStream, reporter, mod_name);

  auto result = psr.Parse();
  programAST = std::move(result);

  if (psr.has_errors())
    set_error();
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
// Thin driver — all orchestration lives in Resolver::Resolve, mirroring the
// parse() stage. Recursion, deduplication, and link-artifact tracking are
// the Resolver's responsibility.
void Compiler::resolve_imports() {
  SAMMINE_ZONE_NAMED("imports");
  if (has_error() || programAST->imports.empty())
    return;

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start resolve imports stage...\n");
  });

  auto source_dir = std::filesystem::path(options_.file_arg).parent_path();
  if (source_dir.empty())
    source_dir = ".";

  Resolver rs(options_, reporter);
  rs.Resolve(*programAST, source_dir, extra_object_files);

  if (rs.has_errors())
    set_error();
  reporter.report(rs);
}

// Stage 4: Load stdlib/definitions.mn (builtin type defs). Executables only.
void Compiler::load_definitions() {
  SAMMINE_ZONE_NAMED("definitions");
  if (has_error() || !has_main)
    return;

  // Find definitions.mn in stdlib_dir
  std::filesystem::path def_path;
  if (!options_.stdlib_dir.empty())
    def_path = options_.stdlib_dir / "definitions.mn";

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
  SAMMINE_ZONE_NAMED("semantics");
  {
    if (has_error()) {
      return;
    }
    LOG({
      fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
                 "Start scope checking stage...\n");
    });
    auto vs = sammine_lang::AST::ScopeGeneratorVisitor();

    programAST->accept_vis(&vs);
    reporter.report(vs);
    if (vs.has_errors())
      set_error();
  }

  // INFO: GeneralSemanticsVisitor
  {
    if (has_error())
      return;
    LOG({
      fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
                 "Start general semantics stage...\n");
    });
    auto vs = sammine_lang::AST::GeneralSemanticsVisitor();

    programAST->accept_vis(&vs);
    reporter.report(vs);
    if (vs.has_errors())
      set_error();
  }
}

// Stage 6: Bidirectional type checking + monomorphization.
// Infers types bottom-up, checks assignments/calls, instantiates generics.
// Monomorphized defs are injected at the front of DefinitionVec for codegen.
void Compiler::typecheck() {
  SAMMINE_ZONE_NAMED("typecheck");
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
  if (vs.has_errors())
    set_error();
}

// Stage 7: Linear type checking — ensures 'ptr<T> values are consumed exactly
// once.
void Compiler::linear_check() {
  SAMMINE_ZONE_NAMED("linear_check");
  if (has_error())
    return;
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start linear type checking stage...\n");
  });
  auto lc = sammine_lang::AST::LinearTypeChecker();
  lc.check(programAST.get(), props_);
  reporter.report(lc);
  if (lc.has_errors())
    set_error();
}

void Compiler::dump_ast() {
  if (options_.ast_ir) {
    LOG({
      fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
                 "Start dumping ast-ir stage...\n");
    });
    AST::ASTPrinter::print(programAST.get(), props_);
  }
  if (has_error()) {
    LOG({
      fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
                 "There were errors in previous stages. Aborting now\n");
    });
    return;
  }
}
void Compiler::codegen() {
  SAMMINE_ZONE_NAMED("codegen");
  if (has_error()) {
    return;
  }
  if (options_.check) {
    LOG({
      fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
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
// from AST via mlirGen(), then lowers MLIR→LLVM IR. Transfers data
// layout/triple from the template LLVMRes module to the newly lowered module.
void Compiler::codegen_mlir() {
  // Check if any kernel definitions exist in the AST.
  bool has_kernel_defs = std::any_of(
      programAST->DefinitionVec.begin(), programAST->DefinitionVec.end(),
      [](const auto &def) { return llvm::isa<AST::KernelDefAST>(def.get()); });

  mlir::MLIRContext mlirCtx;

  // GPU: register all LLVM conversion extensions BEFORE loading any dialects.
  // This ensures ConvertToLLVMPatternInterface is available when dialects load.
  bool target_gpu = options_.gpu != GPUMode::NONE;
  if (target_gpu) {
    mlir::DialectRegistry earlyRegistry;
    mlir::registerAllExtensions(earlyRegistry);
    mlir::registerConvertToLLVMDependentDialectLoading(earlyRegistry);
    mlir::gpu::registerOffloadingLLVMTranslationInterfaceExternalModels(
        earlyRegistry);
    mlirCtx.appendDialectRegistry(earlyRegistry);
  }

  // Core dialects — always needed.
  mlirCtx.getOrLoadDialect<mlir::arith::ArithDialect>();
  mlirCtx.getOrLoadDialect<mlir::func::FuncDialect>();
  mlirCtx.getOrLoadDialect<mlir::LLVM::LLVMDialect>();
  mlirCtx.getOrLoadDialect<mlir::scf::SCFDialect>();
  mlirCtx.getOrLoadDialect<mlir::cf::ControlFlowDialect>();

  // Kernel dialects — only needed when kernel defs exist.
  if (has_kernel_defs) {
    mlirCtx.getOrLoadDialect<mlir::linalg::LinalgDialect>();
    mlirCtx.getOrLoadDialect<mlir::tensor::TensorDialect>();
    mlirCtx.getOrLoadDialect<mlir::bufferization::BufferizationDialect>();
    mlirCtx.getOrLoadDialect<mlir::affine::AffineDialect>();
    mlirCtx.getOrLoadDialect<mlir::memref::MemRefDialect>();
  }

  // GPU dialect — needed when --gpu=cuda or --gpu=amd.
  if (target_gpu) {
    mlirCtx.getOrLoadDialect<mlir::gpu::GPUDialect>();
  }

  mlir::DialectRegistry registry;
  // Core bufferization interfaces — always needed.
  mlir::arith::registerBufferizableOpInterfaceExternalModels(registry);
  mlir::arith::registerBufferDeallocationOpInterfaceExternalModels(registry);
  mlir::bufferization::func_ext::registerBufferizableOpInterfaceExternalModels(
      registry);
  mlir::cf::registerBufferizableOpInterfaceExternalModels(registry);
  mlir::cf::registerBufferDeallocationOpInterfaceExternalModels(registry);
  mlir::scf::registerBufferizableOpInterfaceExternalModels(registry);
  mlir::scf::registerBufferDeallocationOpInterfaceExternalModels(registry);

  // Kernel bufferization interfaces — only when needed.
  if (has_kernel_defs) {
    mlir::linalg::registerBufferizableOpInterfaceExternalModels(registry);
    mlir::tensor::registerBufferizableOpInterfaceExternalModels(registry);
    mlir::memref::registerAllocationOpInterfaceExternalModels(registry);
  }
  // GPU conversion interfaces — gpu-to-llvm uses ConvertToLLVM internally,
  // which requires each dialect to register its ConvertToLLVMPatternInterface.
  if (target_gpu) {
    mlir::arith::registerConvertArithToLLVMInterface(registry);
    mlir::registerConvertComplexToLLVMInterface(registry);
    mlir::cf::registerConvertControlFlowToLLVMInterface(registry);
    mlir::registerConvertFuncToLLVMInterface(registry);
    mlir::gpu::registerConvertGpuToLLVMInterface(registry);
    mlir::index::registerConvertIndexToLLVMInterface(registry);
    mlir::registerConvertMemRefToLLVMInterface(registry);
    mlir::registerConvertMathToLLVMInterface(registry);
    mlir::registerConvertNVVMToLLVMInterface(registry);
    mlir::ub::registerConvertUBToLLVMInterface(registry);
    mlir::vector::registerConvertVectorToLLVMInterface(registry);
    mlir::NVVM::registerNVVMTargetInterfaceExternalModels(registry);
  }
  mlirCtx.appendDialectRegistry(registry);

  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
  std::string moduleName = has_main ? "" : stem;

  auto mlirResult =
      mlirGen(mlirCtx, programAST.get(), moduleName, options_.file_arg,
              options_.str_arg, props_, options_.gpu);
  if (!mlirResult.cpuModule) {
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::red),
               "MLIR generation failed\n");
    set_error();
    return;
  }

  if (options_.mlir_ir) {
    mlir::OpPrintingFlags flags;
    flags.enableDebugInfo(/*enable=*/true, /*prettyForm=*/true);
    if (mlirResult.kernelModule) {
      llvm::outs() << "// === Kernel Module ===\n";
      mlirResult.kernelModule->print(llvm::outs(), flags);
      llvm::outs() << "\n";
    }
    llvm::outs() << "// === CPU Module ===\n";
    mlirResult.cpuModule->print(llvm::outs(), flags);
    llvm::outs() << "\n";
    state_ = State::Finished;
    return;
  }

  mlir::ModuleOp kernelMod;
  if (mlirResult.kernelModule)
    kernelMod = *mlirResult.kernelModule;
  auto llvmModule = lowerMLIRToLLVMIR(*mlirResult.cpuModule, kernelMod,
                                      *resPtr->Context, options_.gpu);
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
      if (!ext)
        continue;
      std::string cName = ext->Prototype->functionName.mangled();
      std::string mangled =
          ext->Prototype->functionName.with_module(moduleName).mangled();
      auto *fn = llvmModule->getFunction(cName);
      if (!fn || llvmModule->getNamedValue(mangled))
        continue;

      auto *ptrTy = llvm::PointerType::get(*resPtr->Context, 0);
      auto *resolverTy = llvm::FunctionType::get(ptrTy, false);
      auto *resolver =
          llvm::Function::Create(resolverTy, llvm::Function::InternalLinkage,
                                 "resolve_" + mangled, llvmModule.get());
      auto *entry =
          llvm::BasicBlock::Create(*resPtr->Context, "entry", resolver);
      llvm::IRBuilder<> irBuilder(entry);
      irBuilder.CreateRet(fn);

      llvm::GlobalIFunc::create(fn->getFunctionType(), 0,
                                llvm::GlobalValue::ExternalLinkage, mangled,
                                resolver, llvmModule.get());
    }
  }

  llvmModule->setDataLayout(resPtr->Module->getDataLayout());
  llvmModule->setTargetTriple(resPtr->Module->getTargetTriple());
  resPtr->Module = std::move(llvmModule);
}

// Stage 9: Run LLVM O2 optimization pipeline. Supports --llvm-ir pre/post/diff
// modes.
void Compiler::optimize() {
  SAMMINE_ZONE_NAMED("optimize");
  if (has_error()) {
    return;
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start optimize stage...\n");
  });
  std::string llvm_ir_mode = options_.llvm_ir;

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
      if (!pre_file.is_open()) {
        llvm::errs() << "Could not open file for writing: " << pre_path << "\n";
        return;
      }
      pre_file << pre_ir;
    }
    {
      std::ofstream post_file(post_path);
      if (!post_file.is_open()) {
        llvm::errs() << "Could not open file for writing: " << post_path
                     << "\n";
        return;
      }
      post_file << post_ir;
    }

    std::string diff_cmd = fmt::format("diff --color -u {} {} 1>&2",
                                       pre_path.string(), post_path.string());
    int diff_ret = std::system(diff_cmd.c_str());
    if (diff_ret != 0 && diff_ret != 1) {
      // diff returns 1 when files differ (expected), >1 on error
      llvm::errs() << "diff command failed with exit code: " << diff_ret
                   << "\n";
    }

    std::filesystem::remove(pre_path);
    std::filesystem::remove(post_path);
  }
}

// JIT execute: when --jit is set and main() exists, run the program directly
// via ORC JIT instead of emitting object files and linking.
void Compiler::jit_execute() {
  SAMMINE_ZONE_NAMED("jit_execute");
  if (has_error())
    return;
  if (!options_.jit)
    return;

  if (!has_main) {
    reportee.add_diagnostics(Location::NonPrintable(),
                             "--jit flag ignored: no main function found. "
                             "JIT execution requires a main function.");
    return;
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start JIT execution stage...\n");
  });

  auto &jit = resPtr->sammineJIT;
  jit_exit_code_ =
      jit->execute_main(std::move(resPtr->Module), std::move(resPtr->Context),
                        extra_object_files, options_.jit_args);
  reporter.report(*jit);

  if (jit->has_errors())
    set_error();
  else
    state_ = State::Finished;
}

// Stage 10: Emit .o object file via LLVM TargetMachine.
void Compiler::emit_object() {
  SAMMINE_ZONE_NAMED("emit_object");
  if (has_error()) {
    return;
  }

  if (options_.from_string) {
    reportee.add_diagnostics(
        Location::NonPrintable(),
        "Skipping object file and executable emission: input was provided "
        "via --str. Use -f to compile from a file.");
    return;
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit object stage...\n");
  });

  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
  std::string obj_file = output_path(stem + ".o");
  llvm::raw_fd_ostream dest(llvm::raw_fd_ostream(obj_file, resPtr->EC));
  if (resPtr->EC) {
    llvm::errs() << "Could not open file: " << resPtr->EC.message() << "\n";
    set_error();
    return;
  }
  auto FileType = llvm::CodeGenFileType::ObjectFile;

  if (resPtr->target_machine->addPassesToEmitFile(resPtr->pass, dest, nullptr,
                                                  FileType)) {
    llvm::errs() << "TargetMachine can't emit a file of this type\n";
    set_error();
    return;
  }

  resPtr->pass.run(*resPtr->Module);
  dest.flush();
}

// Create a static archive (.a) by collecting this module's .o + transitive
// deps.
void Compiler::emit_archive_impl() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit archive stage...\n");
  });

  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
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
  if (result != 0) {
    fmt::print(stderr, "Failed to create archive {}\n", archive_file);
    set_error();
  }

  // Clean up temp directory
  std::filesystem::remove_all(tmp_dir);
}

// Create a shared library (.so) via clang++ -shared.
void Compiler::emit_shared_impl() {
  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start emit shared library stage...\n");
  });

  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
  std::string obj_file = output_path(stem + ".o");
  std::string lib_file = output_path(stem + ".so");

  std::string extra;
  for (auto &obj : extra_link_objs_)
    extra += " " + obj;
  std::string platform_flags;
#ifdef __APPLE__
  platform_flags = " -undefined dynamic_lookup";
#endif
  std::string command = fmt::format("clang++ -shared{} -o {} {}{}",
                                    platform_flags, lib_file, obj_file, extra);
  int result = std::system(command.c_str());
  if (result != 0) {
    fmt::print(stderr, "Failed to create shared library {}\n", lib_file);
    set_error();
  }
}

void Compiler::emit_library() {
  if (has_error() || has_main || options_.from_string)
    return;

  // Default to shared library when no --lib flag is given

  // Auto-detect <stem>_runtime.o for library builds.
  {
    std::string stem = std::filesystem::path(options_.file_arg).stem().string();
    std::string runtime_obj = stem + "_runtime.o";
    bool found = false;
    if (!options_.output_dir.empty()) {
      // TODO: use output_path
      auto candidate = options_.output_dir / runtime_obj;
      if (std::filesystem::exists(candidate)) {
        extra_link_objs_.push_back(candidate.string());
        found = true;
      }
    }
    if (!found) {
      // TODO: use output_path
      auto source_dir = std::filesystem::path(options_.file_arg).parent_path();
      if (source_dir.empty())
        source_dir = ".";
      auto candidate = source_dir / runtime_obj;
      if (std::filesystem::exists(candidate))
        extra_link_objs_.push_back(candidate.string());
    }
  }

  switch (options_.lib_format) {
  case LibFormat::Static:
    emit_archive_impl();
    break;
  case LibFormat::Shared:
    emit_shared_impl();
    break;
  }

  // Clean up intermediate .o — only the .a/.so is the deliverable
  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
  std::string obj_file = output_path(stem + ".o");
  std::filesystem::remove(obj_file);
}

// Stage 12: Link .o files into executable via clang++ (fallback: g++).
// Executables only.
void Compiler::link() {
  SAMMINE_ZONE_NAMED("link");
  if (has_error() || options_.from_string) {
    return;
  }

  LOG({
    fmt::print(stderr, sammine_util::styled(fmt::terminal_color::green),
               "Start link stage...\n");
  });

  std::string stem = std::filesystem::path(options_.file_arg).stem().string();
  std::string obj_file = output_path(stem + ".o");
  std::string exe_file = output_path(stem + ".exe");

  std::string obj_list = obj_file;
  for (auto &obj : extra_object_files)
    obj_list += " " + obj;

  // GPU: link against MLIR CUDA runtime wrappers (mgpu* functions)
  bool gpu = options_.gpu != GPUMode::NONE;
  std::string gpu_link_flags;
  if (gpu) {
    // libmlir_cuda_runtime.so provides mgpuMemAlloc, mgpuMemcpy, etc.
    // MLIR_LLVM_LIB_DIR is set by cmake from the MLIR_DIR config path.
    std::string lib_dir = MLIR_LLVM_LIB_DIR;
    gpu_link_flags = fmt::format(" -L{} -lmlir_cuda_runtime -Wl,-rpath,{}",
                                 lib_dir, lib_dir);
  }

  auto try_compile_with = [&obj_list, &exe_file,
                           &gpu_link_flags](const std::string &compiler) {
    std::string test_command =
        fmt::format("{} --version", compiler) + " > /dev/null 2>&1";
    int test_result = std::system(test_command.c_str());
    if (test_result != 0)
      return false;
    std::string command = fmt::format("{} {} -o {}{}", compiler, obj_list,
                                      exe_file, gpu_link_flags);
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
  SAMMINE_ZONE_NAMED("compiler_pipeline");
  if (has_error())
    return 1;
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
      {"jit_execute", &Compiler::jit_execute},
      {"emit_obj", &Compiler::emit_object},
      {"emit_library", &Compiler::emit_library},
      {"link", &Compiler::link},
  };

  auto time_level = options_.time_val;
  bool timing = options_.time_val != TimingMode::NONE;
  std::vector<std::pair<const char *, double>> timings;

  if (time_level == TimingMode::COARSE)
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
    if (time_level == TimingMode::SIMPLE) {
      double total = 0.0;
      for (auto &[name, ms] : timings)
        total += ms;
      fmt::print(stderr, "total: {:.2f}ms\n", total);
    } else {
      print_timing_table(timings);
      if (time_level == TimingMode::COARSE)
        llvm::TimerGroup::printAll(llvm::errs());
    }
  }

  if (reportee.has_message())
    reporter.report(reportee);

  if (has_error())
    return 1;
  if (options_.jit && has_main)
    return jit_exit_code_;
  return 0;
}

} // namespace sammine_lang
