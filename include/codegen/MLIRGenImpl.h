#pragma once

#include "ast/Ast.h"
#include "typecheck/Types.h"
#include "util/Utilities.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <string>

namespace sammine_lang {

// Named constants for magic strings used across MLIR codegen
static constexpr llvm::StringLiteral kClosureTypeName = "sammine.closure";
static constexpr llvm::StringLiteral kStructTypePrefix = "sammine.struct.";
static constexpr llvm::StringLiteral kWrapperPrefix = "__wrap_";
static constexpr llvm::StringLiteral kPartialPrefix = "__partial_";
static constexpr llvm::StringLiteral kStringPrefix = ".str.";
static constexpr llvm::StringLiteral kMallocFunc = "malloc";
static constexpr llvm::StringLiteral kFreeFunc = "free";
static constexpr llvm::StringLiteral kExitFunc = "exit";

/// Implementation class for MLIR generation from a sammine AST.
/// Declared in a header so method definitions can be split across
/// MLIRGen.cpp, MLIRGenFunction.cpp, and MLIRGenExpr.cpp.
class MLIRGenImpl {
public:
  MLIRGenImpl(mlir::MLIRContext &context, const std::string &moduleName,
              const std::string &fileName, const std::string &sourceText)
      : builder(&context), moduleName(moduleName), fileName(fileName),
        diagnosticData(
            sammine_util::Reporter::get_diagnostic_data(sourceText)) {}

  mlir::ModuleOp generate(AST::ProgramAST *program);

  // --- Member variables ---

  mlir::ModuleOp theModule;
  mlir::OpBuilder builder;
  std::string moduleName;
  std::string fileName;
  sammine_util::Reporter::DiagnosticData diagnosticData;

  AST::LexicalStack<mlir::Value, std::monostate> symbolTable;

  int strCounter = 0;

  std::map<std::string, mlir::LLVM::LLVMStructType> structTypes;
  std::map<std::string, mlir::LLVM::LLVMStructType> enumTypes;

  mlir::LLVM::LLVMStructType closureType;
  std::map<std::string, std::string> closureWrappers;
  int partialCounter = 0;

  mlir::Value currentSretBuffer = nullptr;

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
  bool isFloatType(const Type &type);
  bool isBoolType(const Type &type);

  // --- Definition emission (MLIRGen.cpp) ---
  void emitDefinition(AST::DefinitionAST *def);
  mlir::FunctionType buildFuncType(AST::PrototypeAST *proto);
  std::string mangleName(const sammine_util::QualifiedName &qn) const;

  // --- Expression dispatcher + block/vardef (MLIRGen.cpp) ---
  mlir::Value emitExpr(AST::ExprAST *ast);
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
  mlir::Value emitIndexExpr(AST::IndexExprAST *ast);
  mlir::Value emitPtrArrayGEP(mlir::Value ptr, mlir::Value idx,
                              const ArrayType &arrType,
                              mlir::Location location);
  mlir::Value emitPtrArrayLoad(mlir::Value ptr, mlir::Value idx,
                               const ArrayType &arrType,
                               mlir::Location location);
  void emitPtrArrayStore(mlir::Value ptr, mlir::Value idx, mlir::Value val,
                         const ArrayType &arrType, mlir::Location location);
  mlir::Value emitLenExpr(AST::LenExprAST *ast);
  mlir::Value emitDerefExpr(AST::DerefExprAST *ast);
  mlir::Value emitAddrOfExpr(AST::AddrOfExprAST *ast);
  mlir::Value emitAllocExpr(AST::AllocExprAST *ast);
  mlir::Value emitFreeExpr(AST::FreeExprAST *ast);
  mlir::Value emitStructLiteralExpr(AST::StructLiteralExprAST *ast);
  mlir::Value emitFieldAccessExpr(AST::FieldAccessExprAST *ast);
  mlir::Value emitEnumConstructor(AST::CallExprAST *ast);
  void emitBoundsCheck(mlir::Value idx, size_t arrSize,
                       mlir::Location location);
  mlir::Value emitArrayComparison(mlir::Value lhs, mlir::Value rhs,
                                  const Type &arrType, TokenType tok,
                                  mlir::Location location);

  // --- Helpers (MLIRGen.cpp) ---
  mlir::Value emitAllocaOne(mlir::Type elemType, mlir::Location loc);
  int64_t getTypeSize(const Type &type);
  mlir::Value getOrCreateGlobalString(llvm::StringRef name,
                                      llvm::StringRef value,
                                      mlir::Location location);
};

} // namespace sammine_lang
