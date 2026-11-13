#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "codegen/MIRLowering.h"
#include "codegen/SourceLocator.h"
#include "compiler/Options.h"
#include "util/Utilities.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/StringMap.h"

#include <string>

//! \file KernelGen.h
//! \brief A kernel's code: its linalg function, `__kernel_<name>`, in a
//! module of its own (tensor types), and its wrapper `<name>` in the CPU
//! module, which a sammine call goes through.

namespace sammine_lang {

class KernelGen {
public:
  /// Builds with `builder`, the one `mir` lowers a kernel's scalar code with.
  /// A kernel's wrapper goes in `cpuModule`; one in a library (`library`)
  /// can be called from other modules.
  KernelGen(mlir::OpBuilder &builder, mlir::ModuleOp cpuModule,
            MIRLowering &mir, const AST::ASTProperties &props,
            const SourceLocator &locator, sammine_util::Reporter &reporter,
            GPUMode gpuMode, bool library)
      : builder_(builder), cpuModule_(cpuModule), mir_(mir), props_(props),
        locator_(locator), reporter_(reporter), gpuMode_(gpuMode),
        library_(library) {}

  /// Declares `kd`'s kernel in the kernel module, and in the CPU module, in
  /// memref types, what its wrapper calls; and declares its wrapper, so a
  /// function MIR lowers can call it.
  void declare(AST::KernelDefAST *kd);
  /// Defines `kd`'s kernel and its wrapper.
  void define(AST::KernelDefAST *kd);

  /// The module of kernels, or null if there is none.
  mlir::ModuleOp kernelModule() const { return kernelModule_; }

private:
  mlir::OpBuilder &builder_;
  mlir::ModuleOp cpuModule_;
  mlir::ModuleOp kernelModule_;
  MIRLowering &mir_;
  const AST::ASTProperties &props_;
  const SourceLocator &locator_;
  sammine_util::Reporter &reporter_;
  GPUMode gpuMode_;
  bool library_;
  /// The kernel being defined's parameters, as its function's arguments.
  llvm::StringMap<mlir::Value> params_;

  bool targetGPU() const { return gpuMode_ != GPUMode::NONE; }
  [[noreturn]] void
  error(const std::string &msg,
        sammine_util::Location loc = sammine_util::Location(-1, -1));
  mlir::Location loc(AST::AstBase *ast) const {
    return locator_.loc(builder_, ast);
  }
  /// The CPU module's symbol for `kd`, its wrapper's.
  std::string publicName(AST::KernelDefAST *kd) const;
  /// The kernel's own function's symbol, in both modules.
  std::string internalName(AST::KernelDefAST *kd) const;

  /// `type` in a kernel: an array is a tensor, or a memref (`asMemref`), as
  /// it is once bufferized; a scalar is itself.
  mlir::Type kernelType(const Type &type, bool asMemref = false);
  /// A kernel's function type. One returning an array takes its result's
  /// tensor or memref last, which it fills.
  mlir::FunctionType kernelFunctionType(AST::PrototypeAST *proto,
                                        bool asMemref = false);
  /// The wrapper's type. On the CPU an array is a pointer to it, a returned
  /// one a buffer passed last; on the GPU an array is a memref, so the
  /// wrapper's body is only memref and gpu ops.
  mlir::FunctionType wrapperFunctionType(AST::PrototypeAST *proto);

  mlir::Value defineMap(AST::KernelMapExprAST *mapExpr, mlir::Location location,
                        mlir::Value output);
  mlir::Value defineReduce(AST::KernelReduceExprAST *reduceExpr,
                           AST::KernelDefAST *kd, mlir::Location location);
  void defineWrapper(AST::KernelDefAST *kd, mlir::Location location);
  /// GPU: frees a device memref.
  void gpuDealloc(mlir::Value deviceMemref, mlir::Location loc);
};

} // namespace sammine_lang
