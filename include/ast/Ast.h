#pragma once
#include "ast/AstBase.h"
#include "ast/AstDecl.h"
#include "util/Utilities.h"
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

//! \file Ast.h
//! \brief Defined the AST Node classes (ProgramAST, RecordDefAST, FuncDefAST)
//! and a visitor interface for traversing the AST

namespace sammine_lang {

namespace AST {

class Printable {};

class TypeExprAST {
public:
  sammine_util::Location location;
  virtual ~TypeExprAST() = default;
  virtual std::string to_string() const = 0;
};

class SimpleTypeExprAST : public TypeExprAST {
public:
  std::string name;
  explicit SimpleTypeExprAST(std::shared_ptr<Token> tok) {
    if (tok) {
      name = tok->lexeme;
      location = tok->get_location();
    }
  }
  std::string to_string() const override { return name; }
};

class PointerTypeExprAST : public TypeExprAST {
public:
  std::unique_ptr<TypeExprAST> pointee;
  explicit PointerTypeExprAST(std::unique_ptr<TypeExprAST> pointee)
      : pointee(std::move(pointee)) {}
  std::string to_string() const override {
    return "ptr<" + pointee->to_string() + ">";
  }
};

class ArrayTypeExprAST : public TypeExprAST {
public:
  std::unique_ptr<TypeExprAST> element;
  size_t size;
  ArrayTypeExprAST(std::unique_ptr<TypeExprAST> element, size_t size)
      : element(std::move(element)), size(size) {}
  std::string to_string() const override {
    return "[" + element->to_string() + ";" + std::to_string(size) + "]";
  }
};

class FunctionTypeExprAST : public TypeExprAST {
public:
  std::vector<std::unique_ptr<TypeExprAST>> paramTypes;
  std::unique_ptr<TypeExprAST> returnType;
  FunctionTypeExprAST(std::vector<std::unique_ptr<TypeExprAST>> paramTypes,
                      std::unique_ptr<TypeExprAST> returnType)
      : paramTypes(std::move(paramTypes)), returnType(std::move(returnType)) {}
  std::string to_string() const override {
    std::string res = "(";
    for (size_t i = 0; i < paramTypes.size(); i++) {
      res += paramTypes[i]->to_string();
      if (i != paramTypes.size() - 1)
        res += ", ";
    }
    res += ") -> " + returnType->to_string();
    return res;
  }
};

class DefinitionAST : public AstBase, public Printable {};

class ProgramAST : public AstBase, public Printable {
public:
  std::vector<std::unique_ptr<DefinitionAST>> DefinitionVec;
  virtual std::string getTreeName() const override { return "ProgramAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

class TypedVarAST : public AstBase, public Printable {
public:
  std::string name;
  bool is_mutable = false;
  std::unique_ptr<TypeExprAST> type_expr;

  explicit TypedVarAST(std::shared_ptr<Token> name,
                       std::unique_ptr<TypeExprAST> type_expr,
                       bool is_mutable = false)
      : is_mutable(is_mutable), type_expr(std::move(type_expr)) {
    this->join_location(name);
    if (name)
      this->name = name->lexeme;
    if (this->type_expr)
      this->join_location(this->type_expr->location);
  }
  explicit TypedVarAST(std::shared_ptr<Token> name, bool is_mutable = false)
      : is_mutable(is_mutable) {
    this->join_location(name);
    if (name)
      this->name = name->lexeme;
  }
  virtual std::string getTreeName() const override { return "TypedVarAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }

  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

//! \brief A prototype to present "func func_name(...) -> type;"

//!
//!
class PrototypeAST : public AstBase, public Printable {
public:
  llvm::Function *function;
  std::string functionName;
  std::unique_ptr<TypeExprAST> return_type_expr;
  std::vector<std::unique_ptr<AST::TypedVarAST>> parameterVectors;
  std::vector<std::string> type_params;
  bool is_generic() const { return !type_params.empty(); }

  explicit PrototypeAST(
      std::shared_ptr<Token> functionName,
      std::unique_ptr<TypeExprAST> return_type_expr,
      std::vector<std::unique_ptr<AST::TypedVarAST>> parameterVectors)
      : return_type_expr(std::move(return_type_expr)) {
    assert(functionName);
    this->functionName = functionName->lexeme;
    this->join_location(functionName);
    if (this->return_type_expr)
      this->join_location(this->return_type_expr->location);

    this->parameterVectors = std::move(parameterVectors);

    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      assert(this->parameterVectors[i]);
    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      this->join_location(this->parameterVectors[i]->get_location());
  }

  explicit PrototypeAST(
      std::shared_ptr<Token> functionName,
      std::vector<std::unique_ptr<AST::TypedVarAST>> parameterVectors) {
    assert(functionName);
    this->functionName = functionName->lexeme;
    this->join_location(functionName);

    this->parameterVectors = std::move(parameterVectors);

    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      assert(this->parameterVectors[i]);
    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      this->join_location(this->parameterVectors[i]->get_location());
  }
  virtual std::string getTreeName() const override { return "PrototypeAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }

  bool returnsUnit() const { return return_type_expr == nullptr; }

  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

//! \brief A Function Definition that has the prototype and definition in terms
//! of a block
class ExternAST : public DefinitionAST {
public:
  std::unique_ptr<PrototypeAST> Prototype;

  ExternAST(std::unique_ptr<PrototypeAST> Prototype)
      : Prototype(std::move(Prototype)) {
    this->join_location(this->Prototype.get());
  }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }

  virtual std::string getTreeName() const override { return "ExternAST"; }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class ExprAST : public AstBase, public Printable {
public:
  bool is_statement = true;
  ~ExprAST() = default;
};

//! \brief An AST to simulate a { } code block
//!
//!
class BlockAST : public AstBase, public Printable {

public:
  std::vector<std::unique_ptr<ExprAST>> Statements;
  virtual std::string getTreeName() const override { return "BlockAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

class FuncDefAST : public DefinitionAST {
public:
  ~FuncDefAST() = default;
  std::unique_ptr<PrototypeAST> Prototype;
  std::unique_ptr<BlockAST> Block;

  FuncDefAST(std::unique_ptr<PrototypeAST> Prototype,
             std::unique_ptr<BlockAST> Block)
      : Prototype(std::move(Prototype)), Block(std::move(Block)) {
    this->join_location(this->Prototype.get())
        ->join_location(this->Block.get());
  }

  virtual std::string getTreeName() const override { return "FuncDefAST"; }

  std::string getFunctionName() const { return Prototype->functionName; }

  bool returnsUnit() const { return Prototype->returnsUnit(); }

  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

// Record id { typed_var }
class RecordDefAST : public DefinitionAST {
public:
  std::string record_name;
  std::vector<std::unique_ptr<TypedVarAST>> record_members;
  virtual std::string getTreeName() const override { return "RecordDefAST"; }

  explicit RecordDefAST(std::shared_ptr<Token> record_id,
                        decltype(record_members) record_members)
      : record_members(std::move(record_members)) {
    if (record_id)
      record_name = record_id->lexeme;

    this->join_location(record_id);
    for (auto &m : record_members)
      this->join_location(m.get());
  }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

//! \brief A variable definition: "var x = expression;"
class VarDefAST : public ExprAST {
public:
  bool is_mutable = false;
  std::unique_ptr<TypedVarAST> TypedVar;
  std::unique_ptr<ExprAST> Expression;

  explicit VarDefAST(std::shared_ptr<Token> let,
                     std::unique_ptr<TypedVarAST> TypedVar,
                     std::unique_ptr<ExprAST> Expression,
                     bool is_mutable = false)
      : is_mutable(is_mutable), TypedVar(std::move(TypedVar)),
        Expression(std::move(Expression)) {

    this->join_location(let)
        ->join_location(this->TypedVar.get())
        ->join_location(this->Expression.get());
  };

  virtual std::string getTreeName() const override { return "VarDefAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class NumberExprAST : public ExprAST {
public:
  std::string number;

  explicit NumberExprAST(std::shared_ptr<Token> t) {
    assert(t);
    join_location(t);
    number = t->lexeme;
  }
  virtual std::string getTreeName() const override { return "NumberExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class StringExprAST : public ExprAST {
public:
  std::string string_content;

  explicit StringExprAST(std::shared_ptr<Token> t) {
    assert(t);
    join_location(t);
    string_content = t->lexeme;
  }
  virtual std::string getTreeName() const override { return "StringExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

class BoolExprAST : public ExprAST {
public:
  bool b;
  BoolExprAST(bool b, sammine_util::Location loc) : b(b) {
    this->location = loc;
  }
  virtual std::string getTreeName() const override { return "BoolExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class BinaryExprAST : public ExprAST {
public:
  std::shared_ptr<Token> Op;
  std::unique_ptr<ExprAST> LHS, RHS;
  BinaryExprAST(std::shared_ptr<Token> op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {
    this->join_location(this->Op)
        ->join_location(this->LHS.get())
        ->join_location(this->RHS.get());
  }

  virtual std::string getTreeName() const override { return "BinaryExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class ReturnExprAST : public ExprAST {

public:
  bool is_implicit;
  std::unique_ptr<ExprAST> return_expr;
  ReturnExprAST(std::shared_ptr<Token> return_tok,
                std::unique_ptr<ExprAST> return_expr)
      : is_implicit(false), return_expr(std::move(return_expr)) {
    if (this->return_expr == nullptr) {
      this->join_location(return_tok);
    } else if (return_tok == nullptr) {
      this->join_location(this->return_expr.get());
    } else if (return_tok && this->return_expr) {
      this->join_location(return_tok)->join_location(this->return_expr.get());
    }
  }

  ReturnExprAST(std::unique_ptr<ExprAST> return_expr)
      : is_implicit(true), return_expr(std::move(return_expr)) {
    this->join_location(this->return_expr.get());
  }

  virtual std::string getTreeName() const override { return "ReturnExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class CallExprAST : public ExprAST {

public:
  std::string functionName;
  std::vector<std::unique_ptr<AST::ExprAST>> arguments;
  std::optional<Type> callee_func_type;
  bool is_partial = false;
  std::optional<std::string> resolved_generic_name;
  std::unordered_map<std::string, Type> type_bindings;
  explicit CallExprAST(
      std::shared_ptr<Token> functionName,
      std::vector<std::unique_ptr<AST::ExprAST>> arguments = {}) {
    join_location(functionName);
    if (functionName)
      this->functionName = functionName->lexeme;

    for (auto &arg : arguments)
      if (arg)
        this->join_location(arg.get());
    this->arguments = std::move(arguments);
  }

  virtual std::string getTreeName() const override { return "CallExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

class UnitExprAST : public ExprAST {

public:
  bool is_implicit;

  explicit UnitExprAST(std::shared_ptr<Token> left_paren,
                       std::shared_ptr<Token> right_paren)
      : is_implicit(false) {
    assert(left_paren);
    assert(right_paren);
    this->join_location(left_paren)->join_location(right_paren);
  };
  explicit UnitExprAST() : is_implicit(true) {}

  virtual std::string getTreeName() const override { return "UnitExpr"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class IfExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> bool_expr;
  std::unique_ptr<BlockAST> thenBlockAST, elseBlockAST;
  explicit IfExprAST(std::unique_ptr<ExprAST> bool_expr,
                     std::unique_ptr<BlockAST> thenBlockAST,
                     std::unique_ptr<BlockAST> elseBlockAST)
      : bool_expr(std::move(bool_expr)), thenBlockAST(std::move(thenBlockAST)),
        elseBlockAST(std::move(elseBlockAST)) {
    this->join_location(this->bool_expr.get())
        ->join_location(this->thenBlockAST.get())
        ->join_location(this->elseBlockAST.get());
  }

  virtual std::string getTreeName() const override { return "IfExpr"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class VariableExprAST : public ExprAST {
public:
  std::string variableName;
  VariableExprAST(std::shared_ptr<Token> var) {
    join_location(var);
    if (var)
      variableName = var->lexeme;
  };

  virtual std::string getTreeName() const override { return "VariableExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class DerefExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit DerefExprAST(std::shared_ptr<Token> star_tok,
                        std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(star_tok)->join_location(this->operand.get());
  }
  virtual std::string getTreeName() const override { return "DerefExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

class AddrOfExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit AddrOfExprAST(std::shared_ptr<Token> amp_tok,
                         std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(amp_tok)->join_location(this->operand.get());
  }
  virtual std::string getTreeName() const override { return "AddrOfExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class AllocExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit AllocExprAST(std::shared_ptr<Token> tok,
                        std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  virtual std::string getTreeName() const override { return "AllocExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

class FreeExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit FreeExprAST(std::shared_ptr<Token> tok,
                       std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  virtual std::string getTreeName() const override { return "FreeExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};
class ArrayLiteralExprAST : public ExprAST {
public:
  std::vector<std::unique_ptr<ExprAST>> elements;
  explicit ArrayLiteralExprAST(std::vector<std::unique_ptr<ExprAST>> elements)
      : elements(std::move(elements)) {
    for (auto &e : this->elements)
      if (e) this->join_location(e.get());
  }
  virtual std::string getTreeName() const override { return "ArrayLiteralExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

class IndexExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> array_expr;
  std::unique_ptr<ExprAST> index_expr;
  explicit IndexExprAST(std::unique_ptr<ExprAST> array_expr,
                        std::unique_ptr<ExprAST> index_expr)
      : array_expr(std::move(array_expr)), index_expr(std::move(index_expr)) {
    this->join_location(this->array_expr.get())
        ->join_location(this->index_expr.get());
  }
  virtual std::string getTreeName() const override { return "IndexExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

class LenExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit LenExprAST(std::shared_ptr<Token> tok,
                      std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  virtual std::string getTreeName() const override { return "LenExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

class UnaryNegExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit UnaryNegExprAST(std::shared_ptr<Token> op_tok,
                           std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(op_tok)->join_location(this->operand.get());
  }
  virtual std::string getTreeName() const override { return "UnaryNegExprAST"; }
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }
  virtual void walk_with_preorder(ASTVisitor *visitor) override {
    visitor->preorder_walk(this);
  }
  virtual void walk_with_postorder(ASTVisitor *visitor) override {
    visitor->postorder_walk(this);
  }
  virtual Type accept_synthesis(TypeCheckerVisitor *visitor) override {
    return visitor->synthesize(this);
  }
};

} // namespace AST
} // namespace sammine_lang
