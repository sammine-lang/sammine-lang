#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "codegen/KernelGen.h"
#include "codegen/MIRLowering.h"
#include "codegen/MIRTypes.h"
#include "codegen/SourceLocator.h"
#include "codegen/TypeLayout.h"
#include "compiler/Compiler.h"
#include "typecheck/Types.h"
#include "util/Utilities.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <string>

namespace sammine_lang {

// Named constants for magic strings used across MLIR codegen
static constexpr llvm::StringLiteral kMallocFunc = "malloc";
static constexpr llvm::StringLiteral kFreeFunc = "free";
static constexpr llvm::StringLiteral kExitFunc = "exit";
static constexpr llvm::StringLiteral kDprintfFunc = "dprintf";

/// Builds a program's MLIR module: its types, its functions' declarations,
/// whose bodies MIRLowering gives them, its C bindings, and its kernels.
/// Split across MLIRGen.cpp and MLIRGenFunction.cpp; kernels are KernelGen's.
class MLIRGenImpl {
public:
  MLIRGenImpl(mlir::MLIRContext &context, const std::string &moduleName_,
              const std::string &fileName_, const std::string &sourceText,
              const AST::ASTProperties &props, GPUMode gpu_mode)
      : builder(&context), moduleName(moduleName_),
        locator(fileName_, sourceText), reporter(fileName_, sourceText, 3),
        types(&context), gpu_mode_(gpu_mode), props_(props) {
    mirLowering = std::make_unique<MIRLowering>(builder, props_, locator);
  }

  mlir::ModuleOp generate(AST::ProgramAST *program);
  /// Gives each function its body through MIR; one MIR can't lower is an
  /// error.
  void lowerThroughMIR(AST::ProgramAST *program);

  mlir::ModuleOp theModule;
  mlir::OpBuilder builder;
  std::string moduleName;
  SourceLocator locator;
  sammine_util::Reporter reporter;
  /// Lowers each function's body, and kernels' scalar code.
  std::unique_ptr<MIRLowering> mirLowering;
  /// Each sammine type's MIR type, what the module declares functions in.
  MIRTypes types;
  /// Kernels' code, and the module of kernels.
  std::unique_ptr<KernelGen> kernels;

  GPUMode gpu_mode_;

  /// Emit an ariadne-style error before aborting. If a Location is provided,
  /// the error points to the offending source span; otherwise shows "In
  /// <file>".
  [[noreturn]] void
  imm_error(const std::string &msg,
            sammine_util::Location loc = sammine_util::Location(-1, -1)) {
    reporter.immediate_error(msg, loc);
    sammine_util::abort(msg);
  }

  const AST::ASTProperties &props_;

  // --- Inline type helpers ---
  mlir::LLVM::LLVMPointerType llvmPtrTy() {
    return mlir::LLVM::LLVMPointerType::get(builder.getContext());
  }
  mlir::LLVM::LLVMVoidType llvmVoidTy() {
    return mlir::LLVM::LLVMVoidType::get(builder.getContext());
  }

  // --- Location helpers ---
  mlir::Location loc(AST::AstBase *ast);

  // --- Runtime function declarations ---
  void declareRuntimeFunctions();

  // --- Types ---
  /// `proto`'s type in MIR types, which sammine-mir-to-llvm converts.
  mlir::FunctionType mirFunctionType(AST::PrototypeAST *proto);
  /// The LLVM type of `type`, through its MIR type: for a C function's
  /// declaration, which has to be in LLVM's types already.
  mlir::Type llvmType(const Type &type);

  // --- Definition emission (MLIRGen.cpp) ---
  void emitDefinition(AST::DefinitionAST *def);
  /// The symbol `qn` is defined and called as, as NameResolver decided it.
  std::string mangleName(const sammine_util::QualifiedName &qn) const;

  // --- Functions and externs (MLIRGenFunction.cpp) ---
  void forwardDeclareFunc(AST::PrototypeAST *proto);
  /// Who a function emitted here is for.
  enum class Linkage {
    /// Other modules link to it. In a library that is every function and
    /// instance method, private ones too, since the library's generic code
    /// compiled in another module may call them; privacy only limits who can
    /// name them. In an executable only main is.
    Global,
    /// Only this module: an executable's functions, and every module's own
    /// copy of a generic instantiation.
    Internal,
    /// A copy of another module's function, an imported instance's method,
    /// for the optimizer to inline; never emitted, as the module defines it.
    AvailableExternally,
  };
  /// The linkage of the functions a function or instance definition makes.
  Linkage functionLinkage(AST::DefinitionAST *def);
  void emitFunction(AST::FuncDefAST *ast, Linkage linkage);
  void emitExtern(AST::ExternAST *ast);
};

} // namespace sammine_lang
