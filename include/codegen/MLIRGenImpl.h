#pragma once

#include "ast/Ast.h"
#include "ast/ASTProperties.h"
#include "typecheck/Types.h"
#include "util/Utilities.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

#include "llvm/ADT/StringRef.h"

#include <functional>
#include <map>
#include <string>

namespace sammine_lang {

// Named constants for magic strings used across MLIR codegen
static constexpr llvm::StringLiteral kClosureTypeName = "sammine.closure";
static constexpr llvm::StringLiteral kStructTypePrefix = "sammine.struct.";
static constexpr llvm::StringLiteral kWrapperPrefix = "__wrap_";
static constexpr llvm::StringLiteral kPartialPrefix = "__partial_";
static constexpr llvm::StringLiteral kStringPrefix = ".str.";
static constexpr llvm::StringLiteral kConstArrayPrefix = ".const_arr.";
static constexpr llvm::StringLiteral kKernelPrefix = "__kernel_";
static constexpr llvm::StringLiteral kMallocFunc = "malloc";
static constexpr llvm::StringLiteral kFreeFunc = "free";
static constexpr llvm::StringLiteral kExitFunc = "exit";

/// Implementation class for MLIR generation from a sammine AST.
/// Declared in a header so method definitions can be split across
/// MLIRGen.cpp, MLIRGenFunction.cpp, and MLIRGenExpr.cpp.
class MLIRGenImpl {
public:
  MLIRGenImpl(mlir::MLIRContext &context, const std::string &moduleName,
              const std::string &fileName, const std::string &sourceText,
              const AST::ASTProperties &props)
      : builder(&context), moduleName(moduleName), fileName(fileName),
        diagnosticData(
            sammine_util::Reporter::get_diagnostic_data(sourceText)),
        reporter(fileName, sourceText, 3),
        props_(props) {}

  mlir::ModuleOp generate(AST::ProgramAST *program);

  // --- Member variables ---

  mlir::ModuleOp theModule;
  /// Separate module for kernel functions (tensor/linalg dialect only).
  /// Created lazily when the first KernelDefAST is encountered.
  mlir::ModuleOp kernelModule;
  mlir::OpBuilder builder;
  std::string moduleName;
  std::string fileName;
  sammine_util::Reporter::DiagnosticData diagnosticData;
  sammine_util::Reporter reporter;

  /// Emit an ariadne-style error before aborting. If a Location is provided,
  /// the error points to the offending source span; otherwise shows "In <file>".
  [[noreturn]] void
  imm_error(const std::string &msg,
            sammine_util::Location loc = sammine_util::Location(-1, -1)) {
    reporter.immediate_error(msg, loc);
    sammine_util::abort(msg);
  }

  AST::LexicalStack<mlir::Value> symbolTable;

  int strCounter = 0;
  int constArrayCounter = 0;

  std::map<std::string, mlir::LLVM::LLVMStructType> structTypes;
  std::map<std::string, mlir::LLVM::LLVMStructType> enumTypes;

  mlir::LLVM::LLVMStructType closureType;
  std::map<std::string, std::string> closureWrappers;
  int partialCounter = 0;

  mlir::Value currentSretBuffer = nullptr;

  const AST::ASTProperties &props_;

  /// Flag: true when emitting inside a linalg.map/linalg.reduce body builder.
  /// Guards against emitting LLVM ops (alloca, malloc, free) that are invalid
  /// inside linalg body regions. Only arith/math ops are valid there.
  bool in_kernel_lambda_body = false;

  // --- Block helper ---
  /// Create a new block and immediately insert it into the given region.
  mlir::Block *addBlockTo(mlir::Region *region) {
    auto *block = new mlir::Block();
    region->push_back(block);
    return block;
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
  bool isIntegerType(const Type &type);
  bool isUnsignedIntegerType(const Type &type);
  bool isFloatType(const Type &type);
  bool isBoolType(const Type &type);
  /// Emit an integer constant with the given value and sammine type.
  mlir::Value emitIntConstant(int64_t value, const Type &type,
                              mlir::Location loc);
  /// Emit a float constant with the given value and sammine type.
  /// Handles F32 precision conversion via APFloat.
  mlir::Value emitFloatConstant(double value, const Type &type,
                                mlir::Location loc);
  mlir::Type getEnumBackingMLIRType(const EnumType &et);

  // --- Kernel type conversion ---
  /// Convert a sammine Type to an MLIR type for kernel context.
  /// Arrays become RankedTensorType (asMemref=false) or MemRefType (asMemref=true).
  mlir::Type convertTypeForKernel(const Type &type, bool asMemref = false);
  /// Build a func::FunctionType for a kernel function.
  /// asMemref=false: tensor types (for __kernel_ pre-bufferization).
  /// asMemref=true:  memref types (post-bufferization, used by wrapper).
  mlir::FunctionType buildKernelFuncType(AST::PrototypeAST *proto,
                                         bool asMemref = false);

  // --- Definition emission (MLIRGen.cpp) ---
  void emitDefinition(AST::DefinitionAST *def);
  void emitKernelDef(AST::KernelDefAST *kd);
  void emitKernelMapExpr(AST::KernelMapExprAST *mapExpr,
                         mlir::Block &entryBlock,
                         AST::KernelDefAST *kd,
                         mlir::Location location,
                         mlir::Value dpsOutput);
  void emitKernelReduceExpr(AST::KernelReduceExprAST *reduceExpr,
                            mlir::Block &entryBlock,
                            AST::KernelDefAST *kd,
                            mlir::Location location);
  void emitKernelWrapper(AST::KernelDefAST *kd,
                         const std::string &internalName,
                         const std::string &publicName,
                         mlir::Location location);
  mlir::FunctionType buildFuncType(AST::PrototypeAST *proto);
  std::string mangleName(const sammine_util::QualifiedName &qn) const;

  // --- Expression dispatcher + block/vardef (MLIRGen.cpp) ---
  mlir::Value emitExpr(AST::ExprAST *ast);
  /// emitExpr + load if the result is a pointer but the semantic type is not.
  mlir::Value emitRValue(AST::ExprAST *ast);
  /// Returns the address of the expression (for assignment LHS, &expr).
  mlir::Value emitLValue(AST::ExprAST *ast);
  mlir::Value emitBlock(AST::BlockAST *ast);
  mlir::Value emitVarDef(AST::VarDefAST *ast);

  // --- Functions/externs/closures/calls (MLIRGenFunction.cpp) ---
  void forwardDeclareFunc(AST::PrototypeAST *proto);
  void emitFunction(AST::FuncDefAST *ast);
  void emitExtern(AST::ExternAST *ast);
  mlir::Value buildClosure(mlir::Value codePtr, mlir::Value envPtr,
                           mlir::Location loc);
  mlir::LLVM::LLVMFunctionType getClosureFuncType(const FunctionType &ft);
  std::string getOrCreateClosureWrapper(const std::string &funcName,
                                        const FunctionType &ft);
  mlir::Value emitCallExpr(AST::CallExprAST *ast);
  mlir::Value emitPartialApplication(AST::CallExprAST *ast,
                                     const std::string &calleeName,
                                     llvm::ArrayRef<mlir::Value> boundArgs);
  mlir::Value emitIndirectCall(AST::CallExprAST *ast,
                               llvm::ArrayRef<mlir::Value> operands);
  void emitFuncCallAndLLVMReturn(llvm::StringRef callee, const Type &retType,
                                 mlir::ValueRange args, mlir::Location loc);
  mlir::Value emitReturnExpr(AST::ReturnExprAST *ast);

  // --- Expression emission (MLIRGenExpr.cpp) ---
  mlir::Value emitNumberExpr(AST::NumberExprAST *ast);
  mlir::Value emitBoolExpr(AST::BoolExprAST *ast);
  mlir::Value emitCharExpr(AST::CharExprAST *ast);
  mlir::Value emitUnitExpr(AST::UnitExprAST *ast);
  mlir::Value emitVariableExpr(AST::VariableExprAST *ast);
  mlir::Value emitBinaryExpr(AST::BinaryExprAST *ast);
  mlir::Value emitUnaryNegExpr(AST::UnaryNegExprAST *ast);
  mlir::Value emitIfExpr(AST::IfExprAST *ast);
  mlir::Value emitWhileExpr(AST::WhileExprAST *ast);
  mlir::Value emitStringExpr(AST::StringExprAST *ast);
  mlir::Value emitArrayLiteralExpr(AST::ArrayLiteralExprAST *ast);
  mlir::Value emitRangeExpr(AST::RangeExprAST *ast);
  mlir::Value emitIndexExpr(AST::IndexExprAST *ast);
  mlir::Value emitPtrArrayGEP(mlir::Value ptr, mlir::Value idx,
                              const ArrayType &arrType,
                              mlir::Location location);
  mlir::Value emitPtrArrayLoad(mlir::Value ptr, mlir::Value idx,
                               const ArrayType &arrType,
                               mlir::Location location);
  void emitPtrArrayStore(mlir::Value ptr, mlir::Value idx, mlir::Value val,
                         const ArrayType &arrType, mlir::Location location);
  mlir::Value emitPtrElementGEP(mlir::Value ptr, mlir::Value idx,
                                 const Type &pointeeType,
                                 mlir::Location location);
  mlir::Value emitLenExpr(AST::LenExprAST *ast);
  mlir::Value emitDimExpr(AST::DimExprAST *ast);
  mlir::Value emitDerefExpr(AST::DerefExprAST *ast);
  mlir::Value emitAddrOfExpr(AST::AddrOfExprAST *ast);
  mlir::Value emitAllocExpr(AST::AllocExprAST *ast);
  mlir::Value emitFreeExpr(AST::FreeExprAST *ast);
  mlir::Value emitStructLiteralExpr(AST::StructLiteralExprAST *ast);
  mlir::Value emitFieldAccessExpr(AST::FieldAccessExprAST *ast);
  mlir::Value emitEnumConstructor(AST::CallExprAST *ast);
  mlir::Value emitCaseExpr(AST::CaseExprAST *ast);
  mlir::Value emitTupleLiteralExpr(AST::TupleLiteralExprAST *ast);
  mlir::Value emitIntegerBackedCaseExpr(AST::CaseExprAST *ast,
                                        mlir::Value scrutineeVal,
                                        const EnumType &et);
  mlir::Value emitLiteralCaseExpr(AST::CaseExprAST *ast,
                                  mlir::Value scrutineeVal);

  /// Given a case arm, produce the integer constant that the scrutinee should
  /// be compared against.
  ///
  /// - emitIntegerBackedCaseExpr passes a lambda that looks up the enum
  ///   variant's discriminant value  (e.g. HUP(1) → 1).
  /// - emitLiteralCaseExpr passes a lambda that parses the source literal
  ///   (e.g. "42" → 42, "true" → 1).
  using ArmToComparisonConst =
      std::function<mlir::Value(const AST::CaseArm &, mlir::Location)>;

  /// Shared codegen for case expressions where every arm boils down to
  /// "compare scrutinee against an integer constant".  This covers both
  /// integer-backed enums and literal (i32/bool/char) patterns.
  ///
  /// Emits a cascading chain of:
  ///     if (scrutinee == armConst) goto armBlock; else goto nextCheck;
  /// where `armConst` is produced by the caller-supplied callback.
  mlir::Value emitScalarCaseExpr(AST::CaseExprAST *ast,
                                 mlir::Value scrutineeVal,
                                 ArmToComparisonConst armToConst);

  void emitBoundsCheck(mlir::Value idx, size_t arrSize,
                       mlir::Location location);
  mlir::Value emitArrayComparison(mlir::Value lhs, mlir::Value rhs,
                                  const Type &arrType, TokenType tok,
                                  mlir::Location location);

  // --- Helpers (MLIRGen.cpp) ---
  /// Build a memref descriptor (LLVM struct) wrapping a raw pointer.
  /// Returns a memref<sizexelemType> via unrealized_conversion_cast.
  mlir::Value buildMemrefFromPtr(mlir::Value ptr, int64_t size,
                                 mlir::Type elemType, mlir::Location loc);
  mlir::Value emitAllocaOne(mlir::Type elemType, mlir::Location loc);
  /// If val is not !llvm.ptr, spill to a temp alloca and return the pointer.
  mlir::Value ensurePointer(mlir::Value val, mlir::Location loc);
  int64_t getTypeSize(const Type &type);
  int64_t getVariantPayloadSize(const std::vector<Type> &payload_types);
  int64_t advancePayloadOffset(int64_t &byte_offset, const Type &field_type);
  mlir::Value getOrCreateGlobalString(llvm::StringRef name,
                                      llvm::StringRef value,
                                      mlir::Location location);
  mlir::Value emitGlobalConstArray(AST::ArrayLiteralExprAST *arrLit,
                                   const Type &type, mlir::Location location);
};

} // namespace sammine_lang
