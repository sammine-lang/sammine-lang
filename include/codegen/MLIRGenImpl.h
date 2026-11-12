#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "codegen/MIRLowering.h"
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
static constexpr llvm::StringLiteral kClosureTypeName = "sammine.closure";
static constexpr llvm::StringLiteral kStructTypePrefix = "sammine.struct.";
static constexpr llvm::StringLiteral kKernelPrefix = "__kernel_";
static constexpr llvm::StringLiteral kMallocFunc = "malloc";
static constexpr llvm::StringLiteral kFreeFunc = "free";
static constexpr llvm::StringLiteral kExitFunc = "exit";
static constexpr llvm::StringLiteral kDprintfFunc = "dprintf";

/// Builds a program's MLIR module: its types, its functions' declarations,
/// whose bodies MIRLowering gives them, its C bindings, and its kernels.
/// Split across MLIRGen.cpp, MLIRGenFunction.cpp and MLIRGenKernel.cpp.
class MLIRGenImpl {
public:
  MLIRGenImpl(mlir::MLIRContext &context, const std::string &moduleName_,
              const std::string &fileName_, const std::string &sourceText,
              const AST::ASTProperties &props, GPUMode gpu_mode)
      : builder(&context), moduleName(moduleName_),
        locator(fileName_, sourceText), reporter(fileName_, sourceText, 3),
        gpu_mode_(gpu_mode), props_(props) {
    mirLowering = std::make_unique<MIRLowering>(builder, props_, locator);
  }

  mlir::ModuleOp generate(AST::ProgramAST *program);
  /// Gives each function its body through MIR; one MIR can't lower is an
  /// error.
  void lowerThroughMIR(AST::ProgramAST *program);

  mlir::ModuleOp theModule;
  /// Separate module for kernel functions (tensor/linalg dialect only).
  /// Created lazily when the first KernelDefAST is encountered.
  mlir::ModuleOp kernelModule;
  mlir::OpBuilder builder;
  std::string moduleName;
  SourceLocator locator;
  sammine_util::Reporter reporter;
  /// Lowers each function's body, and kernels' scalar code.
  std::unique_ptr<MIRLowering> mirLowering;

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

  AST::LexicalStack<mlir::Value> symbolTable;

  std::unordered_map<Type, mlir::LLVM::LLVMStructType, std::hash<Type>>
      structTypes;
  std::unordered_map<Type, mlir::LLVM::LLVMStructType, std::hash<Type>>
      enumTypes;

  mlir::LLVM::LLVMStructType closureType;

  const AST::ASTProperties &props_;
  bool targetGPU() const {
    return gpu_mode_ != GPUMode::NONE;
  }

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

  // --- Type conversion ---
  mlir::Type convertType(const Type &type);
  bool isUnsignedIntegerType(const Type &type);
  bool isFloatType(const Type &type);
  mlir::Type getEnumBackingMLIRType(const EnumType &et);

  // --- Kernel type conversion ---
  /// Convert a sammine Type to an MLIR type for kernel context.
  /// Arrays become RankedTensorType (asMemref=false) or MemRefType
  /// (asMemref=true).
  mlir::Type convertTypeForKernel(const Type &type, bool asMemref = false);
  /// Build a func::FunctionType for a kernel function.
  /// asMemref=false: tensor types (for __kernel_ pre-bufferization).
  /// asMemref=true:  memref types (post-bufferization, used by wrapper).
  mlir::FunctionType buildKernelFuncType(AST::PrototypeAST *proto,
                                         bool asMemref = false);

  // --- Definition emission (MLIRGen.cpp) ---
  void emitDefinition(AST::DefinitionAST *def);
  void emitKernelDef(AST::KernelDefAST *kd);
  mlir::Value emitKernelMapExpr(AST::KernelMapExprAST *mapExpr,
                                AST::KernelDefAST *kd, mlir::Location location,
                                mlir::Value dpsOutput);
  mlir::Value emitKernelReduceExpr(AST::KernelReduceExprAST *reduceExpr,
                                   AST::KernelDefAST *kd,
                                   mlir::Location location);
  void emitKernelWrapper(AST::KernelDefAST *kd, const std::string &internalName,
                         const std::string &publicName,
                         mlir::Location location);
  /// Build function type for the GPU kernel wrapper.
  /// Array params become memref<NxT> (not !llvm.ptr) so the wrapper body
  /// can use pure memref/gpu dialect ops without LLVM dialect mixing.
  mlir::FunctionType buildGpuWrapperFuncType(AST::PrototypeAST *proto);
  /// GPU: allocate device memref and copy host data into it.
  mlir::Value gpuCopyToDevice(mlir::Value hostMemref, mlir::Location loc);
  /// GPU: copy device memref back to host and deallocate device memory.
  void gpuCopyToHostAndDealloc(mlir::Value deviceMemref,
                               mlir::Value hostMemref, mlir::Location loc);
  /// GPU: deallocate a device memref.
  void gpuDealloc(mlir::Value deviceMemref, mlir::Location loc);
  mlir::FunctionType buildFuncType(AST::PrototypeAST *proto);
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
