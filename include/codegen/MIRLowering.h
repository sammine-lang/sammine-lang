#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "codegen/SourceLocator.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <optional>

//! \file MIRLowering.h
//! \brief Lowers a type-checked function's body to MIR (see
//! knowledge_base/plans/mir.md). MIR grows one construct at a time: a body
//! using anything not handled yet is left to the AST codegen.

namespace sammine_lang {

class MIRLowering {
public:
  MIRLowering(mlir::OpBuilder &builder, const AST::ASTProperties &props,
              const SourceLocator &locator)
      : builder_(builder), props_(props), locator_(locator) {}

  /// Lowers `ast` to a new function with its MIR signature and body, marked
  /// `smn::kMirAttr`, which replaces `decl`, the AST codegen's declaration of
  /// it. Returns false, leaving `decl` untouched, if `ast` uses something MIR
  /// can't lower yet.
  bool lowerFunction(AST::FuncDefAST *ast, mlir::func::FuncOp decl);

private:
  mlir::OpBuilder &builder_;
  const AST::ASTProperties &props_;
  const SourceLocator &locator_;
  /// The module the function being lowered goes in, where its callees are.
  mlir::ModuleOp module_;
  /// What each name in the current function stands for: its parameters and
  /// `let`s so far. A later `let` of a name shadows the earlier one.
  llvm::StringMap<mlir::Value> names_;

  mlir::Location loc(const AST::AstBase *ast) const {
    return locator_.loc(builder_, ast);
  }
  /// The MIR type of a sammine type MIR handles so far, or nullopt.
  std::optional<mlir::Type> mirType(const Type &type) const;
  /// As above, inside the structs named in `enclosing`.
  std::optional<mlir::Type> mirType(const Type &type,
                                    llvm::StringSet<> &enclosing) const;
  /// A function type in MIR types, if MIR has a type for each part. A C
  /// function's `...` has none: this is what it declares.
  std::optional<mlir::FunctionType>
  mirFunctionType(const FunctionType &fn) const;
  /// `ast`'s signature in MIR types, if MIR has a type for each part.
  std::optional<mlir::FunctionType> mirSignature(AST::FuncDefAST *ast) const;

  /// Whether the block being built has ended, as after a return.
  bool terminated() const;
  mlir::FailureOr<mlir::Value> lowerStatements(AST::BlockAST *block);
  mlir::LogicalResult lowerVarDef(AST::VarDefAST *def);
  mlir::LogicalResult lowerReturn(AST::ReturnStmtAST *ret);
  /// The value of `expr`, or a null value for an expression of type `()`.
  mlir::FailureOr<mlir::Value> lowerExpr(AST::ExprAST *expr);
  mlir::FailureOr<mlir::Value> lowerCall(AST::CallExprAST *call);
  mlir::FailureOr<mlir::Value> lowerArgument(AST::ExprAST *arg,
                                             mlir::Type param);
  mlir::FailureOr<mlir::Value> lowerVariadicArgument(AST::ExprAST *arg);
  /// A view of what the owned pointer `owner` points at, for a call.
  mlir::Value lend(mlir::Value owner, mlir::Location loc);
  mlir::FailureOr<mlir::Value>
  lowerAggregate(mlir::Location location, mlir::Type type,
                 llvm::ArrayRef<mlir::Type> fieldTypes,
                 llvm::ArrayRef<std::pair<size_t, AST::ExprAST *>> exprs);
  mlir::FailureOr<mlir::Value>
  lowerStructLiteral(AST::StructLiteralExprAST *lit);
  mlir::FailureOr<mlir::Value>
  lowerTupleLiteral(AST::TupleLiteralExprAST *tuple);
  mlir::FailureOr<mlir::Value>
  lowerFieldAccess(AST::FieldAccessExprAST *access);
  mlir::FailureOr<mlir::Value> lowerAlloc(AST::AllocExprAST *alloc);
  mlir::FailureOr<mlir::Value> lowerFree(AST::FreeExprAST *free);
  mlir::FailureOr<mlir::Value> lowerDerefPlace(AST::DerefExprAST *deref);
  mlir::FailureOr<mlir::Value> lowerDerefRead(AST::DerefExprAST *deref);
  mlir::FailureOr<mlir::Value> lowerAssign(AST::BinaryExprAST *assign);
  mlir::FailureOr<mlir::Value> lowerNumber(AST::NumberExprAST *num);
  mlir::FailureOr<mlir::Value> lowerString(AST::StringExprAST *str);
  mlir::FailureOr<mlir::Value> lowerBool(AST::BoolExprAST *b);
  mlir::FailureOr<mlir::Value> lowerChar(AST::CharExprAST *c);
  mlir::FailureOr<mlir::Value> lowerVariable(AST::VariableExprAST *var);
  mlir::FailureOr<mlir::Value> lowerBinary(AST::BinaryExprAST *bin);
  mlir::FailureOr<mlir::Value> lowerLogical(AST::BinaryExprAST *bin);
  mlir::FailureOr<mlir::Value> lowerIf(AST::IfExprAST *ifExpr);
};

} // namespace sammine_lang
