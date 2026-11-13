#pragma once

#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "codegen/MIRTypes.h"
#include "codegen/SourceLocator.h"
#include "dialect/SammineDialect.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <functional>
#include <optional>

//! \file MIRLowering.h
//! \brief Lowers a type-checked function's body to MIR (see
//! knowledge_base/plans/mir.md), and a kernel's scalar code. Every function
//! body goes through it.

namespace sammine_lang {

class MIRLowering {
public:
  MIRLowering(mlir::OpBuilder &builder, const AST::ASTProperties &props,
              const SourceLocator &locator)
      : builder_(builder), props_(props), locator_(locator),
        types_(builder.getContext()) {}

  /// Function types carry what `layouts` says their closures bind.
  void setClosureLayouts(const ClosureLayouts *layouts) {
    types_.setClosureLayouts(layouts);
  }

  /// Lowers `ast` to a new function with its MIR signature and body, marked
  /// `smn::kMirAttr`, which replaces `decl`, its declaration. Returns false,
  /// leaving `decl` untouched, if `ast` uses something MIR can't lower.
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
  /// Each sammine type's MIR type.
  MIRTypes types_;
  /// The module the function being lowered goes in, where its callees are.
  mlir::ModuleOp module_;
  /// What each name in the current function stands for: its parameters and
  /// `let`s so far. A later `let` of a name shadows the earlier one.
  llvm::StringMap<mlir::Value> names_;
  /// The bindings of the function being lowered whose address its code
  /// takes: each lives in a slot, and names_ names that place.
  llvm::StringSet<> addressed_;

  /// Restores names_ when it ends: the names made in a block, an arm or a
  /// loop's body end with it.
  class NameScope {
  public:
    explicit NameScope(MIRLowering &lowering)
        : lowering_(lowering), outer_(lowering.names_) {}
    ~NameScope() { lowering_.names_ = std::move(outer_); }
    NameScope(const NameScope &) = delete;
    NameScope &operator=(const NameScope &) = delete;

  private:
    MIRLowering &lowering_;
    llvm::StringMap<mlir::Value> outer_;
  };

  mlir::Value intConstant(mlir::Location location, mlir::Type type,
                          int64_t value);
  /// The type of a place holding a `type`.
  smn::PlaceType placeOf(mlir::Type type) const;
  /// The type of a view of a `pointee`.
  smn::PtrType viewOf(mlir::Type pointee) const;
  /// Moves `block` to the end of its region and builds from its start, as
  /// where control continues once the blocks before it are built.
  void continueIn(mlir::Block *block);

  /// Makes `name` name `value`, or a slot holding it if it is addressed.
  void bind(llvm::StringRef name, mlir::Value value, mlir::Location location);
  bool hasPlace(AST::ExprAST *expr) const;
  std::optional<size_t> fieldIndex(AST::FieldAccessExprAST *access) const;
  mlir::FailureOr<mlir::Value> lowerAddrOf(AST::AddrOfExprAST *addr);
  mlir::FailureOr<mlir::Value> lowerIndirectCall(AST::CallExprAST *call);
  mlir::FailureOr<mlir::Value> lowerPartialApplication(AST::CallExprAST *call,
                                                       llvm::StringRef name,
                                                       mlir::FunctionType type);
  mlir::FailureOr<mlir::Value> lowerFunctionValue(AST::VariableExprAST *var);
  mlir::FailureOr<mlir::Value>
  lowerIndexerCall(AST::IndexExprAST *index,
                   const sammine_util::QualifiedName &method);

  mlir::Location loc(const AST::AstBase *ast) const {
    return locator_.loc(builder_, ast);
  }
  mlir::Location loc(const sammine_util::Location &location) const {
    return locator_.loc(builder_, location);
  }
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
  /// The value of `expr`, failing for one of type `()`.
  mlir::FailureOr<mlir::Value> lowerValue(AST::ExprAST *expr);
  mlir::FailureOr<mlir::Value> lowerNegation(AST::UnaryNegExprAST *neg);
  mlir::FailureOr<mlir::Value> lowerLen(AST::LenExprAST *len);
  mlir::FailureOr<mlir::Value> lowerCall(AST::CallExprAST *call);
  mlir::FailureOr<mlir::Value> lowerArgument(AST::ExprAST *arg,
                                             mlir::Type param);
  mlir::FailureOr<mlir::Value> lowerVariadicArgument(AST::ExprAST *arg);
  /// A view of `place`, what an owned pointer points at, lent to a call or
  /// a closure.
  mlir::Value lend(mlir::Value place, mlir::Location loc);
  /// The place what `pointer` names points at: through a pointer value, or
  /// through one held in a place, which stays there.
  mlir::FailureOr<mlir::Value> lowerPointee(AST::ExprAST *pointer,
                                            mlir::Location loc);
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
  /// A call of `method` standing for the syntax `written`, as `as`.
  callMethod(mlir::Location location, const sammine_util::QualifiedName &method,
             mlir::ValueRange args, const Type &result,
             llvm::StringRef written);
  mlir::FailureOr<mlir::Value> lowerBuiltinInstance(mlir::Location location,
                                                    const MonomorphizedKey &key,
                                                    mlir::ValueRange args,
                                                    const Type &result);
  mlir::FailureOr<mlir::Value> lowerCast(AST::CastExprAST *cast);
  mlir::FailureOr<mlir::Value>
  lowerArrayLiteral(AST::ArrayLiteralExprAST *array);
  mlir::FailureOr<mlir::Value> lowerArrayIndex(AST::IndexExprAST *index);
  mlir::FailureOr<mlir::Value> lowerRangeArray(AST::RangeExprAST *range);
  mlir::FailureOr<mlir::Value> lowerDim(AST::DimExprAST *dim);
  mlir::LogicalResult checkBounds(mlir::Location location, mlir::Value offset,
                                  int64_t size);
  /// Prints `format`, with `args`, to stderr through dprintf, then exit(1):
  /// the block ends, unreachable. Fails if the runtime isn't declared.
  mlir::LogicalResult fail(mlir::Location location, llvm::StringRef format,
                           mlir::ValueRange args);
  mlir::FailureOr<mlir::Value> lowerPanic(AST::PanicExprAST *panic);
  mlir::FailureOr<mlir::Value> lowerAlloc(AST::AllocExprAST *alloc);
  mlir::FailureOr<mlir::Value> lowerDrop(AST::DropExprAST *free);
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
  /// An arm of an if, a case, `&&` or `||`: the block it starts in, and
  /// what it leaves, lowered there in a scope of its own (binding what its
  /// pattern binds, if anything, first).
  struct Arm {
    mlir::Block *block;
    std::function<mlir::FailureOr<mlir::Value>()> value;
  };
  /// An arm that is `body`, after `bind`, if any.
  Arm armOf(mlir::Block *block, AST::BlockAST *body,
            std::function<void()> bind = nullptr);
  mlir::FailureOr<std::optional<mlir::Type>>
  branchResultType(const Type &type) const;
  mlir::FailureOr<mlir::Value> lowerArms(mlir::Location location,
                                         std::optional<mlir::Type> resultType,
                                         llvm::ArrayRef<Arm> arms);
  /// `cond` picks `then` or `otherwise`, each lowered in a block of its own;
  /// the value is the one the arm taken leaves, of `resultType`.
  mlir::FailureOr<mlir::Value>
  lowerBranch(mlir::Location location, mlir::Value cond,
              std::optional<mlir::Type> resultType,
              std::function<mlir::FailureOr<mlir::Value>()> then,
              std::function<mlir::FailureOr<mlir::Value>()> otherwise);
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
