#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "codegen/SourceLocator.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <functional>
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

  /// A kernel's scalar code where the builder is: the body of a map's lambda
  /// or a reduce's identity, `bindings` naming the lambda's parameters. It
  /// must lower to plain arith ops in the one block a linalg region has:
  /// failure, with nothing left behind, if it doesn't.
  mlir::FailureOr<mlir::Value>
  lowerKernelScalar(AST::BlockAST *body,
                    const llvm::StringMap<mlir::Value> &bindings);
  mlir::FailureOr<mlir::Value>
  lowerKernelScalar(AST::ExprAST *expr,
                    const llvm::StringMap<mlir::Value> &bindings);

private:
  mlir::OpBuilder &builder_;
  const AST::ASTProperties &props_;
  const SourceLocator &locator_;
  /// The module the function being lowered goes in, where its callees are.
  mlir::ModuleOp module_;
  /// What each name in the current function stands for: its parameters and
  /// `let`s so far. A later `let` of a name shadows the earlier one.
  llvm::StringMap<mlir::Value> names_;
  /// The bindings of the function being lowered whose address its code
  /// takes: each lives in a slot, and names_ names that place.
  llvm::StringSet<> addressed_;

  /// Makes `name` name `value`, or a slot holding it if it is addressed.
  void bind(llvm::StringRef name, mlir::Value value, mlir::Location location);
  bool hasPlace(AST::ExprAST *expr) const;
  std::optional<size_t> fieldIndex(AST::FieldAccessExprAST *access) const;
  mlir::FailureOr<mlir::Value> lowerAddrOf(AST::AddrOfExprAST *addr);
  bool callableFromMIR(mlir::FunctionOpInterface callee,
                       mlir::FunctionType type) const;
  mlir::FailureOr<mlir::Value> lowerIndirectCall(AST::CallExprAST *call);
  mlir::FailureOr<mlir::Value>
  lowerPartialApplication(AST::CallExprAST *call, llvm::StringRef name,
                          mlir::FunctionType type);
  mlir::FailureOr<mlir::Value>
  lowerFunctionValue(AST::VariableExprAST *var);
  mlir::FailureOr<mlir::Value>
  lowerIndexerCall(AST::IndexExprAST *index,
                   const sammine_util::QualifiedName &method);

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

  mlir::FailureOr<mlir::Value>
  lowerArithOnly(const llvm::StringMap<mlir::Value> &bindings,
                 llvm::function_ref<mlir::FailureOr<mlir::Value>()> lower);
  /// Whether the block being built has ended, as after a return.
  bool terminated() const;
  mlir::FailureOr<mlir::Value> lowerStatements(AST::BlockAST *block);
  mlir::LogicalResult lowerVarDef(AST::VarDefAST *def);
  mlir::LogicalResult lowerReturn(AST::ReturnStmtAST *ret);
  /// The value of `expr`, or a null value for an expression of type `()`.
  mlir::FailureOr<mlir::Value> lowerExpr(AST::ExprAST *expr);
  mlir::FailureOr<mlir::Value> lowerCall(AST::CallExprAST *call);
  /// How long a view of what an owned pointer points at lasts: a call
  /// (sammine.lend), or as long as what it points at (sammine.addr), as a
  /// view a partial application binds does.
  enum class ViewLasts { Call, Place };
  mlir::FailureOr<mlir::Value>
  lowerArgument(AST::ExprAST *arg, mlir::Type param,
                ViewLasts view = ViewLasts::Call);
  mlir::FailureOr<mlir::Value> lowerVariadicArgument(AST::ExprAST *arg);
  /// A view of what the owned pointer `owner` points at.
  mlir::Value lend(mlir::Value owner, mlir::Location loc, ViewLasts view);
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
  mlir::FailureOr<mlir::Value>
  callMethod(mlir::Location location, const sammine_util::QualifiedName &method,
             mlir::ValueRange args, const Type &result);
  mlir::FailureOr<mlir::Value>
  lowerBuiltinInstance(mlir::Location location, const MonomorphizedKey &key,
                       mlir::ValueRange args, const Type &result);
  mlir::FailureOr<mlir::Value> lowerCast(AST::CastExprAST *cast);
  mlir::FailureOr<mlir::Value>
  lowerArrayLiteral(AST::ArrayLiteralExprAST *array);
  mlir::FailureOr<mlir::Value> lowerArrayIndex(AST::IndexExprAST *index);
  mlir::FailureOr<mlir::Value> lowerRangeArray(AST::RangeExprAST *range);
  mlir::FailureOr<mlir::Value> lowerDim(AST::DimExprAST *dim);
  mlir::LogicalResult checkBounds(mlir::Location location, mlir::Value offset,
                                  int64_t size);
  mlir::FailureOr<mlir::Value> lowerAlloc(AST::AllocExprAST *alloc);
  mlir::FailureOr<mlir::Value> lowerFree(AST::FreeExprAST *free);
  mlir::FailureOr<mlir::Value> lowerDerefPlace(AST::DerefExprAST *deref);
  mlir::FailureOr<mlir::Value> lowerPtrElemPlace(AST::IndexExprAST *index);
  mlir::FailureOr<mlir::Value> lowerPlace(AST::ExprAST *target);
  mlir::FailureOr<mlir::Value> lowerPlaceRead(AST::ExprAST *expr);
  /// A copy or a move of what `place` holds, as its type says.
  mlir::Value read(mlir::Location location, mlir::Value place);
  mlir::FailureOr<mlir::Value> lowerAssign(AST::BinaryExprAST *assign);
  mlir::FailureOr<mlir::Value> lowerNumber(AST::NumberExprAST *num);
  mlir::FailureOr<mlir::Value> lowerString(AST::StringExprAST *str);
  mlir::FailureOr<mlir::Value> lowerBool(AST::BoolExprAST *b);
  mlir::FailureOr<mlir::Value> lowerChar(AST::CharExprAST *c);
  mlir::FailureOr<mlir::Value> lowerVariable(AST::VariableExprAST *var);
  mlir::FailureOr<mlir::Value> lowerBinary(AST::BinaryExprAST *bin);
  mlir::FailureOr<mlir::Value> lowerLogical(AST::BinaryExprAST *bin);
  mlir::FailureOr<mlir::Value> lowerOperator(mlir::Location location,
                                             TokenType op, mlir::Value lhs,
                                             mlir::Value rhs, const Type &type);
  static std::optional<bool> integerKind(const Type &type);
  mlir::FailureOr<mlir::Value> lowerEquality(mlir::Location location,
                                             mlir::Value lhs, mlir::Value rhs,
                                             const Type &type);
  /// An arm of an if or a case: the block it starts in, what its pattern
  /// binds (if anything), and its body.
  struct Arm {
    mlir::Block *block;
    AST::BlockAST *body;
    std::function<void()> bind;
  };
  mlir::FailureOr<std::optional<mlir::Type>>
  branchResultType(const Type &type) const;
  mlir::FailureOr<mlir::Value> lowerArms(mlir::Location location,
                                         std::optional<mlir::Type> resultType,
                                         llvm::ArrayRef<Arm> arms);
  mlir::FailureOr<mlir::Value> lowerIf(AST::IfExprAST *ifExpr);
  mlir::FailureOr<mlir::Value> lowerCase(AST::CaseExprAST *caseExpr);
  mlir::FailureOr<mlir::Value> lowerFor(AST::ForExprAST *forExpr);
  mlir::FailureOr<mlir::Value> lowerScalarCase(AST::CaseExprAST *caseExpr);
  void branchOn(mlir::Location location, mlir::Value flag,
                llvm::ArrayRef<llvm::APInt> values,
                llvm::ArrayRef<mlir::Block *> dests, mlir::Block *otherwise,
                mlir::ValueRange otherwiseArgs);
  void branchOn(mlir::Location location, mlir::Value flag,
                llvm::ArrayRef<int32_t> tags,
                llvm::ArrayRef<mlir::Block *> dests, mlir::Block *otherwise,
                mlir::ValueRange otherwiseArgs);
  mlir::FailureOr<mlir::Value>
  lowerVariant(mlir::Location location, const Type &type, size_t index,
               llvm::ArrayRef<AST::ExprAST *> payload);
};

} // namespace sammine_lang
