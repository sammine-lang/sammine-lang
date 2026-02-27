//
// Created by jjasmine on 3/9/24.
//

#pragma once
#include "ast/AstDecl.h"
#include "lex/Lexer.h"
#include "lex/Token.h"
#include "typecheck/Types.h"
#include "util/LexicalContext.h"
#include "util/Utilities.h"
#include "llvm/Support/Casting.h"
#include <stack>

//! \file AstBase.h
//! \brief Defines the AST Abstract class for printing out AST Nodes
namespace llvm {
class Value;

class Function;
} // namespace llvm

namespace sammine_lang {
namespace AST {

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
  UnaryNegExprAST,
  StructLiteralExprAST,
  FieldAccessExprAST,
  LastExpr = FieldAccessExprAST,

  // DefinitionAST subclasses [FirstDef..LastDef]
  FirstDef,
  FuncDefAST = FirstDef,
  ExternAST,
  StructDefAST,
  EnumDefAST,
  TypeClassDeclAST,
  TypeClassInstanceAST,
  LastDef = TypeClassInstanceAST,
};

class Visitable;
class AstBase;
class ASTVisitor;

struct ASTPrinter {
  static void print(AstBase *t);
  static void print(ProgramAST *t);
};
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

  virtual void visit(UnaryNegExprAST *ast);
  virtual void preorder_walk(UnaryNegExprAST *ast) = 0;
  virtual void postorder_walk(UnaryNegExprAST *ast) = 0;

  virtual void visit(StructLiteralExprAST *ast);
  virtual void preorder_walk(StructLiteralExprAST *ast) = 0;
  virtual void postorder_walk(StructLiteralExprAST *ast) = 0;

  virtual void visit(FieldAccessExprAST *ast);
  virtual void preorder_walk(FieldAccessExprAST *ast) = 0;
  virtual void postorder_walk(FieldAccessExprAST *ast) = 0;

  virtual void visit(TypeClassDeclAST *ast);
  virtual void preorder_walk(TypeClassDeclAST *ast) = 0;
  virtual void postorder_walk(TypeClassDeclAST *ast) = 0;

  virtual void visit(TypeClassInstanceAST *ast);
  virtual void preorder_walk(TypeClassInstanceAST *ast) = 0;
  virtual void postorder_walk(TypeClassInstanceAST *ast) = 0;

  virtual ~ASTVisitor() = 0;
};

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

class TypeCheckerVisitor {
public:
  virtual Type synthesize(ProgramAST *ast) = 0;
  virtual Type synthesize(VarDefAST *ast) = 0;
  virtual Type synthesize(ExternAST *ast) = 0;
  virtual Type synthesize(FuncDefAST *ast) = 0;
  virtual Type synthesize(StructDefAST *ast) = 0;
  virtual Type synthesize(EnumDefAST *ast) = 0;
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
  virtual Type synthesize(UnaryNegExprAST *ast) = 0;
  virtual Type synthesize(StructLiteralExprAST *ast) = 0;
  virtual Type synthesize(FieldAccessExprAST *ast) = 0;
  virtual Type synthesize(TypeClassDeclAST *ast) = 0;
  virtual Type synthesize(TypeClassInstanceAST *ast) = 0;

  virtual ~TypeCheckerVisitor() = 0;
};

class Visitable {
public:
  virtual ~Visitable() = default;
  virtual void accept_vis(ASTVisitor *visitor) = 0;
  virtual void walk_with_preorder(ASTVisitor *visitor) = 0;
  virtual void walk_with_postorder(ASTVisitor *visitor) = 0;
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) = 0;
  virtual std::string getTreeName() const = 0;
};

class AstBase : public Visitable {
  NodeKind kind;

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
  AstBase(NodeKind kind) : kind(kind) {}
  NodeKind getKind() const { return kind; }
  // INFO: Parser error
  bool pe = false;
  llvm::Value *val;
  Type type = Type::NonExistent();
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
  bool synthesized() const { return this->type.synthesized(); }
};
} // namespace AST
} // namespace sammine_lang
