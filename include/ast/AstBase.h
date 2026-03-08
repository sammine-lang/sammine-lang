//
// Created by jjasmine on 3/9/24.
//

#pragma once
#include "ast/AstDecl.h"
#include "ast/ASTProperties.h"
#include "lex/Lexer.h"
#include "lex/Token.h"
#include "typecheck/Types.h"
#include "util/LexicalContext.h"
#include "util/Utilities.h"
#include "llvm/Support/Casting.h"
#include <atomic>
#include <stack>

//! \file AstBase.h
//! \brief Defines the AST Abstract class for printing out AST Nodes
namespace llvm {
class Value;

class Function;
} // namespace llvm

namespace sammine_lang {
namespace AST {

/// Discriminator for all AST node types. Used with LLVM-style RTTI (classof).
/// FirstExpr..LastExpr and FirstDef..LastDef ranges enable isa<ExprAST>/isa<DefinitionAST>.
enum class NodeKind {
  // Direct AstBase children (not ExprAST, not DefinitionAST)
  ProgramAST,
  PrototypeAST,
  TypedVarAST,
  BlockAST,

  // ExprAST subclasses [FirstExpr..LastExpr]
  FirstExpr,
  VarDefAST = FirstExpr,
  NumberExprAST,
  StringExprAST,
  BoolExprAST,
  CharExprAST,
  BinaryExprAST,
  CallExprAST,
  ReturnExprAST,
  UnitExprAST,
  VariableExprAST,
  IfExprAST,
  DerefExprAST,
  AddrOfExprAST,
  AllocExprAST,
  FreeExprAST,
  ArrayLiteralExprAST,
  IndexExprAST,
  LenExprAST,
  DimExprAST,
  UnaryNegExprAST,
  StructLiteralExprAST,
  FieldAccessExprAST,
  CaseExprAST,
  WhileExprAST,
  TupleLiteralExprAST,
  LastExpr = TupleLiteralExprAST,

  // DefinitionAST subclasses [FirstDef..LastDef]
  FirstDef,
  FuncDefAST = FirstDef,
  ExternAST,
  StructDefAST,
  EnumDefAST,
  TypeAliasDefAST,
  TypeClassDeclAST,
  TypeClassInstanceAST,
  KernelDefAST,
  LastDef = KernelDefAST,

  // KernelExprAST subclasses [FirstKernelExpr..LastKernelExpr]
  FirstKernelExpr,
  KernelNumberExprAST = FirstKernelExpr,
  KernelMapExprAST,
  KernelReduceExprAST,
  LastKernelExpr = KernelReduceExprAST,
};

class Visitable;
class AstBase;
class ASTVisitor;
class ASTProperties;

struct ASTPrinter {
  static void print(AstBase *t, const ASTProperties &props);
  static void print(ProgramAST *t, const ASTProperties &props);
  // Convenience overloads for debug/error paths that don't have props available
  static void print(AstBase *t);
  static void print(ProgramAST *t);
};
/// Base visitor for AST traversal. Each node type has three methods:
/// - visit(): main dispatch (called by accept_vis)
/// - preorder_walk(): called before visiting children (e.g. ScopeGenerator)
/// - postorder_walk(): called after visiting children
class ASTVisitor : public sammine_util::Reportee {
protected:
  ProgramAST *top_level_ast = nullptr;

public:
  [[noreturn]] virtual void
  abort(const std::string &msg = "<NO MESSAGE>") override final;

  virtual void visit(ProgramAST *ast);
  virtual void preorder_walk(ProgramAST *ast) = 0;
  virtual void postorder_walk(ProgramAST *ast) = 0;

  virtual void visit(VarDefAST *ast);
  virtual void preorder_walk(VarDefAST *ast) = 0;
  virtual void postorder_walk(VarDefAST *ast) = 0;

  virtual void visit(ExternAST *ast);
  virtual void preorder_walk(ExternAST *ast) = 0;
  virtual void postorder_walk(ExternAST *ast) = 0;

  virtual void visit(FuncDefAST *ast);
  virtual void preorder_walk(FuncDefAST *ast) = 0;
  virtual void postorder_walk(FuncDefAST *ast) = 0;

  virtual void visit(StructDefAST *ast);
  virtual void preorder_walk(StructDefAST *ast) = 0;
  virtual void postorder_walk(StructDefAST *ast) = 0;

  virtual void visit(EnumDefAST *ast);
  virtual void preorder_walk(EnumDefAST *ast) = 0;
  virtual void postorder_walk(EnumDefAST *ast) = 0;

  virtual void visit(TypeAliasDefAST *ast);
  virtual void preorder_walk(TypeAliasDefAST *ast) = 0;
  virtual void postorder_walk(TypeAliasDefAST *ast) = 0;

  virtual void visit(PrototypeAST *ast);
  virtual void preorder_walk(PrototypeAST *ast) = 0;
  virtual void postorder_walk(PrototypeAST *ast) = 0;

  virtual void visit(CallExprAST *ast);
  virtual void preorder_walk(CallExprAST *ast) = 0;
  virtual void postorder_walk(CallExprAST *ast) = 0;

  virtual void visit(ReturnExprAST *ast);
  virtual void preorder_walk(ReturnExprAST *ast) = 0;
  virtual void postorder_walk(ReturnExprAST *ast) = 0;

  virtual void visit(BinaryExprAST *ast);
  virtual void preorder_walk(BinaryExprAST *ast) = 0;
  virtual void postorder_walk(BinaryExprAST *ast) = 0;

  virtual void visit(NumberExprAST *ast);
  virtual void preorder_walk(NumberExprAST *ast) = 0;
  virtual void postorder_walk(NumberExprAST *ast) = 0;

  virtual void visit(StringExprAST *ast);
  virtual void preorder_walk(StringExprAST *ast) = 0;
  virtual void postorder_walk(StringExprAST *ast) = 0;

  virtual void visit(BoolExprAST *ast);
  virtual void preorder_walk(BoolExprAST *ast) = 0;
  virtual void postorder_walk(BoolExprAST *ast) = 0;

  virtual void visit(CharExprAST *ast);
  virtual void preorder_walk(CharExprAST *ast) = 0;
  virtual void postorder_walk(CharExprAST *ast) = 0;

  virtual void visit(UnitExprAST *ast);
  virtual void preorder_walk(UnitExprAST *ast) = 0;
  virtual void postorder_walk(UnitExprAST *ast) = 0;

  virtual void visit(VariableExprAST *ast);
  virtual void preorder_walk(VariableExprAST *ast) = 0;
  virtual void postorder_walk(VariableExprAST *ast) = 0;

  virtual void visit(BlockAST *ast);
  virtual void preorder_walk(BlockAST *ast) = 0;
  virtual void postorder_walk(BlockAST *ast) = 0;

  virtual void visit(IfExprAST *ast);
  virtual void preorder_walk(IfExprAST *ast) = 0;
  virtual void postorder_walk(IfExprAST *ast) = 0;

  virtual void visit(TypedVarAST *ast);
  virtual void preorder_walk(TypedVarAST *ast) = 0;
  virtual void postorder_walk(TypedVarAST *ast) = 0;

  virtual void visit(DerefExprAST *ast);
  virtual void preorder_walk(DerefExprAST *ast) = 0;
  virtual void postorder_walk(DerefExprAST *ast) = 0;

  virtual void visit(AddrOfExprAST *ast);
  virtual void preorder_walk(AddrOfExprAST *ast) = 0;
  virtual void postorder_walk(AddrOfExprAST *ast) = 0;

  virtual void visit(AllocExprAST *ast);
  virtual void preorder_walk(AllocExprAST *ast) = 0;
  virtual void postorder_walk(AllocExprAST *ast) = 0;

  virtual void visit(FreeExprAST *ast);
  virtual void preorder_walk(FreeExprAST *ast) = 0;
  virtual void postorder_walk(FreeExprAST *ast) = 0;

  virtual void visit(ArrayLiteralExprAST *ast);
  virtual void preorder_walk(ArrayLiteralExprAST *ast) = 0;
  virtual void postorder_walk(ArrayLiteralExprAST *ast) = 0;

  virtual void visit(IndexExprAST *ast);
  virtual void preorder_walk(IndexExprAST *ast) = 0;
  virtual void postorder_walk(IndexExprAST *ast) = 0;

  virtual void visit(LenExprAST *ast);
  virtual void preorder_walk(LenExprAST *ast) = 0;
  virtual void postorder_walk(LenExprAST *ast) = 0;

  virtual void visit(DimExprAST *ast);
  virtual void preorder_walk(DimExprAST *ast) = 0;
  virtual void postorder_walk(DimExprAST *ast) = 0;

  virtual void visit(UnaryNegExprAST *ast);
  virtual void preorder_walk(UnaryNegExprAST *ast) = 0;
  virtual void postorder_walk(UnaryNegExprAST *ast) = 0;

  virtual void visit(StructLiteralExprAST *ast);
  virtual void preorder_walk(StructLiteralExprAST *ast) = 0;
  virtual void postorder_walk(StructLiteralExprAST *ast) = 0;

  virtual void visit(FieldAccessExprAST *ast);
  virtual void preorder_walk(FieldAccessExprAST *ast) = 0;
  virtual void postorder_walk(FieldAccessExprAST *ast) = 0;

  virtual void visit(CaseExprAST *ast);
  virtual void preorder_walk(CaseExprAST *ast) = 0;
  virtual void postorder_walk(CaseExprAST *ast) = 0;

  virtual void visit(WhileExprAST *ast);
  virtual void preorder_walk(WhileExprAST *ast) = 0;
  virtual void postorder_walk(WhileExprAST *ast) = 0;

  virtual void visit(TupleLiteralExprAST *ast);
  virtual void preorder_walk(TupleLiteralExprAST *ast) = 0;
  virtual void postorder_walk(TupleLiteralExprAST *ast) = 0;

  virtual void visit(TypeClassDeclAST *ast);
  virtual void preorder_walk(TypeClassDeclAST *ast) = 0;
  virtual void postorder_walk(TypeClassDeclAST *ast) = 0;

  virtual void visit(TypeClassInstanceAST *ast);
  virtual void preorder_walk(TypeClassInstanceAST *ast) = 0;
  virtual void postorder_walk(TypeClassInstanceAST *ast) = 0;

  virtual void visit(KernelDefAST *ast);
  virtual void preorder_walk(KernelDefAST *ast) = 0;
  virtual void postorder_walk(KernelDefAST *ast) = 0;

  virtual ~ASTVisitor() = 0;
};

/// Stack of scoped symbol tables with parent-chain lookup.
/// Each context can see names from its parent scope via recursive queries.
/// T = value type (e.g. mlir::Value, Type), S = secondary data (often monostate).
template <class T, class S>
class LexicalStack : public std::stack<LexicalContext<T, S>> {
public:
  void push_context() {
    if (this->empty())
      this->push(LexicalContext<T, S>());
    else
      this->push(LexicalContext<T, S>(&this->top()));
  }
  void pop_context() {
    if (this->empty())
      sammine_util::abort("ICE: You are popping an empty lexical stack");
    this->pop();
  }

  /// RAII guard: pushes on construction, pops on destruction.
  class Guard {
    LexicalStack &stack_;
  public:
    explicit Guard(LexicalStack &stack) : stack_(stack) {
      stack_.push_context();
    }
    ~Guard() { stack_.pop_context(); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
  };

  void registerNameT(const std::string &name, T l) {
    return this->top().registerNameT(name, l);
  }
  NameQueryResult recursiveQueryName(const std::string &name) const {
    return this->top().recursiveQueryName(name);
  }

  T get_from_name(const std::string &name) const {
    return this->top().get_from_name(name);
  }
  NameQueryResult queryName(const std::string &name) const {
    return this->top().queryName(name);
  }

  T recursive_get_from_name(const std::string &name) const {
    return this->top().recursive_get_from_name(name);
  }

  const LexicalContext<T, S> *parent_scope() const {
    return this->top().parent_scope;
  }
  LexicalContext<T, S> *parent_scope() { return this->top().parent_scope; }
};

/// Visitor that manages scope enter/exit around function bodies.
class ScopedASTVisitor : public ASTVisitor {
public:
  virtual void enter_new_scope() = 0;
  virtual void exit_new_scope() = 0;

  /// Only visiting the FuncDefAST requires the use of scoping for now.
  ///
  /// In the future we might have to support BlockAST
  virtual void visit(FuncDefAST *ast);

  virtual ~ScopedASTVisitor() = 0;
};

/// Bidirectional type checker interface. synthesize() infers types bottom-up.
/// Implemented by BiTypeCheckerVisitor.
class TypeCheckerVisitor {
public:
  virtual Type synthesize(ProgramAST *ast) = 0;
  virtual Type synthesize(VarDefAST *ast) = 0;
  virtual Type synthesize(ExternAST *ast) = 0;
  virtual Type synthesize(FuncDefAST *ast) = 0;
  virtual Type synthesize(StructDefAST *ast) = 0;
  virtual Type synthesize(EnumDefAST *ast) = 0;
  virtual Type synthesize(TypeAliasDefAST *ast) = 0;
  virtual Type synthesize(PrototypeAST *ast) = 0;
  virtual Type synthesize(CallExprAST *ast) = 0;
  virtual Type synthesize(ReturnExprAST *ast) = 0;
  virtual Type synthesize(BinaryExprAST *ast) = 0;
  virtual Type synthesize(NumberExprAST *ast) = 0;
  virtual Type synthesize(StringExprAST *ast) = 0;
  virtual Type synthesize(UnitExprAST *ast) = 0;
  virtual Type synthesize(BoolExprAST *ast) = 0;
  virtual Type synthesize(CharExprAST *ast) = 0;
  virtual Type synthesize(VariableExprAST *ast) = 0;
  virtual Type synthesize(BlockAST *ast) = 0;
  virtual Type synthesize(IfExprAST *ast) = 0;
  virtual Type synthesize(TypedVarAST *ast) = 0;
  virtual Type synthesize(DerefExprAST *ast) = 0;
  virtual Type synthesize(AddrOfExprAST *ast) = 0;
  virtual Type synthesize(AllocExprAST *ast) = 0;
  virtual Type synthesize(FreeExprAST *ast) = 0;
  virtual Type synthesize(ArrayLiteralExprAST *ast) = 0;
  virtual Type synthesize(IndexExprAST *ast) = 0;
  virtual Type synthesize(LenExprAST *ast) = 0;
  virtual Type synthesize(DimExprAST *ast) = 0;
  virtual Type synthesize(UnaryNegExprAST *ast) = 0;
  virtual Type synthesize(StructLiteralExprAST *ast) = 0;
  virtual Type synthesize(FieldAccessExprAST *ast) = 0;
  virtual Type synthesize(CaseExprAST *ast) = 0;
  virtual Type synthesize(WhileExprAST *ast) = 0;
  virtual Type synthesize(TupleLiteralExprAST *ast) = 0;
  virtual Type synthesize(TypeClassDeclAST *ast) = 0;
  virtual Type synthesize(TypeClassInstanceAST *ast) = 0;
  virtual Type synthesize(KernelDefAST *ast) = 0;

  virtual ~TypeCheckerVisitor() = 0;
};

/// Double-dispatch interface for AST nodes. Enables visitor pattern
/// without dynamic_cast on the visitor side — each node calls the
/// correct overload via its generated accept_vis/accept_synthesis methods.
class Visitable {
public:
  virtual ~Visitable() = default;
  virtual void accept_vis(ASTVisitor *visitor) = 0;
  virtual void walk_with_preorder(ASTVisitor *visitor) = 0;
  virtual void walk_with_postorder(ASTVisitor *visitor) = 0;
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) = 0;
  virtual std::string getTreeName() const = 0;
};

/// Base class for all AST nodes. Provides LLVM-style RTTI (via NodeKind),
/// source location tracking (via join_location), and type storage (via ASTProperties).
class AstBase : public Visitable {
  NodeKind kind;
  NodeId node_id_;  // unique ID for ASTProperties type lookup

  static inline std::atomic<NodeId> next_id_{0};
  static inline ASTProperties *current_props_ = nullptr; // externalized type storage

  void change_location(sammine_util::Location loc) {
    if (first_location) {
      this->location = loc;
      first_location = false;
    } else
      this->location |= loc;
  }

  bool first_location = true;

protected:
  sammine_util::Location location;

public:
  AstBase(NodeKind kind) : kind(kind), node_id_(next_id_++) {}
  NodeId id() const { return node_id_; }
  static void reset_id_counter() { next_id_ = 0; }
  static void set_properties(ASTProperties *p) { current_props_ = p; }
  NodeKind getKind() const { return kind; }
  bool pe = false;       // parser error: set when a child is null (failed parse)
  llvm::Value *val;      // legacy LLVM codegen value (unused with MLIR backend)
  AstBase *join_location(AstBase *ast) {
    if (!ast)
      pe = true;
    else
      change_location(ast->get_location());

    return this;
  }

  AstBase *join_location(std::shared_ptr<Token> tok) {

    if (!tok)
      pe = true;
    else
      change_location(tok->get_location());

    return this;
  }

  AstBase *join_location(sammine_util::Location location) {
    if (location.source_start <= 0 && location.source_end <= 0)
      return this;

    change_location(location);
    return this;
  }
  sammine_util::Location get_location() const { return this->location; }
  void set_location(sammine_util::Location loc) {
    this->location = loc;
    first_location = false;
  }
  bool synthesized() const { return get_type().synthesized(); }
  Type get_type() const {
    if (current_props_)
      return current_props_->get_type(node_id_);
    return Type::NonExistent();
  }
  Type set_type(const Type &t) {
    if (current_props_)
      current_props_->set_type(node_id_, t);
    return t;
  }
};
} // namespace AST
} // namespace sammine_lang
