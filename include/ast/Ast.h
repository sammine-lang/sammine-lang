#pragma once
#include "ast/AstBase.h"
#include "ast/AstDecl.h"
#include "util/QualifiedName.h"
#include "util/Utilities.h"
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

//! \file Ast.h
//! \brief Defined the AST Node classes (ProgramAST, StructDefAST, FuncDefAST)
//! and a visitor interface for traversing the AST

// clang-format off
#define AST_NODE_METHODS(tree_name, kind_val)                                  \
  std::string getTreeName() const override { return tree_name; }               \
  static bool classof(const AstBase *node) {                                   \
    return node->getKind() == kind_val;                                        \
  }                                                                            \
  void accept_vis(ASTVisitor *visitor) override { visitor->visit(this); }      \
  void walk_with_preorder(ASTVisitor *visitor) override {                      \
    visitor->preorder_walk(this);                                              \
  }                                                                            \
  void walk_with_postorder(ASTVisitor *visitor) override {                     \
    visitor->postorder_walk(this);                                             \
  }                                                                            \
  Type accept_synthesis(TypeCheckerVisitor *visitor) override {                \
    return visitor->synthesize(this);                                          \
  }
// clang-format on

namespace sammine_lang {

namespace AST {

class Printable {};

enum class ParseKind {
  Simple,
  Pointer,
  Array,
  Function,
  Generic,
  Tuple,
};

class TypeExprAST {
  ParseKind kind;
public:
  ParseKind getKind() const { return kind; }
  sammine_util::Location location;
  TypeExprAST(ParseKind kind) : kind(kind) {}
  virtual ~TypeExprAST() = default;
  virtual std::string to_string() const = 0;
};

class SimpleTypeExprAST : public TypeExprAST {
public:
  sammine_util::QualifiedName name;
  explicit SimpleTypeExprAST(std::shared_ptr<Token> tok)
      : TypeExprAST(ParseKind::Simple),
        name(sammine_util::QualifiedName::local(tok ? tok->lexeme : "")) {
    if (tok)
      location = tok->get_location();
  }
  explicit SimpleTypeExprAST(sammine_util::QualifiedName qn,
                             sammine_util::Location loc)
      : TypeExprAST(ParseKind::Simple), name(std::move(qn)) {
    location = loc;
  }
  static bool classof(const TypeExprAST *node) {
    return node->getKind() == ParseKind::Simple;
  }
  std::string to_string() const override { return name.display(); }
};

class PointerTypeExprAST : public TypeExprAST {
public:
  std::unique_ptr<TypeExprAST> pointee;
  bool is_linear = false;
  explicit PointerTypeExprAST(std::unique_ptr<TypeExprAST> pointee,
                              bool is_linear = false)
      : TypeExprAST(ParseKind::Pointer), pointee(std::move(pointee)),
        is_linear(is_linear) {}
  static bool classof(const TypeExprAST *node) {
    return node->getKind() == ParseKind::Pointer;
  }
  std::string to_string() const override {
    return (is_linear ? "'" : "") + std::string("ptr<") + pointee->to_string() + ">";
  }
};

class ArrayTypeExprAST : public TypeExprAST {
public:
  std::unique_ptr<TypeExprAST> element;
  size_t size;
  ArrayTypeExprAST(std::unique_ptr<TypeExprAST> element, size_t size)
      : TypeExprAST(ParseKind::Array), element(std::move(element)),
        size(size) {}
  static bool classof(const TypeExprAST *node) {
    return node->getKind() == ParseKind::Array;
  }
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
      : TypeExprAST(ParseKind::Function),
        paramTypes(std::move(paramTypes)), returnType(std::move(returnType)) {}
  static bool classof(const TypeExprAST *node) {
    return node->getKind() == ParseKind::Function;
  }
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

// Generic type expression: Option<i32>, Result<i32, String>
class GenericTypeExprAST : public TypeExprAST {
public:
  sammine_util::QualifiedName base_name;
  std::vector<std::unique_ptr<TypeExprAST>> type_args;
  GenericTypeExprAST(sammine_util::QualifiedName base_name,
                     std::vector<std::unique_ptr<TypeExprAST>> type_args,
                     sammine_util::Location loc)
      : TypeExprAST(ParseKind::Generic), base_name(std::move(base_name)),
        type_args(std::move(type_args)) {
    location = loc;
  }
  static bool classof(const TypeExprAST *node) {
    return node->getKind() == ParseKind::Generic;
  }
  std::string to_string() const override {
    std::string res = base_name.display() + "<";
    for (size_t i = 0; i < type_args.size(); i++) {
      res += type_args[i]->to_string();
      if (i != type_args.size() - 1)
        res += ", ";
    }
    res += ">";
    return res;
  }
};

class TupleTypeExprAST : public TypeExprAST {
public:
  std::vector<std::unique_ptr<TypeExprAST>> element_types;
  TupleTypeExprAST(std::vector<std::unique_ptr<TypeExprAST>> element_types)
      : TypeExprAST(ParseKind::Tuple),
        element_types(std::move(element_types)) {}
  static bool classof(const TypeExprAST *node) {
    return node->getKind() == ParseKind::Tuple;
  }
  std::string to_string() const override {
    std::string res = "(";
    for (size_t i = 0; i < element_types.size(); i++) {
      res += element_types[i]->to_string();
      if (i != element_types.size() - 1)
        res += ", ";
    }
    res += ")";
    return res;
  }
};

class DefinitionAST : public AstBase, public Printable {
public:
  DefinitionAST(NodeKind kind) : AstBase(kind) {}
  static bool classof(const AstBase *node) {
    return node->getKind() >= NodeKind::FirstDef &&
           node->getKind() <= NodeKind::LastDef;
  }
};

struct ImportDecl {
  std::string module_name;
  std::string alias;
  sammine_util::Location location;
};

class ProgramAST : public AstBase, public Printable {
public:
  ProgramAST() : AstBase(NodeKind::ProgramAST) {}
  std::vector<ImportDecl> imports;
  std::vector<std::unique_ptr<DefinitionAST>> DefinitionVec;
  AST_NODE_METHODS("ProgramAST", NodeKind::ProgramAST)
};

class TypedVarAST : public AstBase, public Printable {
public:
  std::string name;
  bool is_mutable = false;
  std::unique_ptr<TypeExprAST> type_expr;

  explicit TypedVarAST(std::shared_ptr<Token> name,
                       std::unique_ptr<TypeExprAST> type_expr,
                       bool is_mutable = false)
      : AstBase(NodeKind::TypedVarAST), is_mutable(is_mutable),
        type_expr(std::move(type_expr)) {
    this->join_location(name);
    if (name)
      this->name = name->lexeme;
    if (this->type_expr)
      this->join_location(this->type_expr->location);
  }
  explicit TypedVarAST(std::shared_ptr<Token> name, bool is_mutable = false)
      : AstBase(NodeKind::TypedVarAST), is_mutable(is_mutable) {
    this->join_location(name);
    if (name)
      this->name = name->lexeme;
  }
  AST_NODE_METHODS("TypedVarAST", NodeKind::TypedVarAST)
};

//! \brief A prototype to present "func func_name(...) -> type;"

//!
//!
class PrototypeAST : public AstBase, public Printable {
public:
  llvm::Function *function;
  sammine_util::QualifiedName functionName;
  std::unique_ptr<TypeExprAST> return_type_expr;
  std::vector<std::unique_ptr<AST::TypedVarAST>> parameterVectors;
  std::vector<std::string> type_params;
  bool is_var_arg = false;
  bool is_generic() const { return !type_params.empty(); }

  explicit PrototypeAST(
      std::shared_ptr<Token> functionName,
      std::unique_ptr<TypeExprAST> return_type_expr,
      std::vector<std::unique_ptr<AST::TypedVarAST>> parameterVectors)
      : AstBase(NodeKind::PrototypeAST),
        return_type_expr(std::move(return_type_expr)) {
    assert(functionName);
    this->functionName = sammine_util::QualifiedName::local(functionName->lexeme);
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
      std::vector<std::unique_ptr<AST::TypedVarAST>> parameterVectors)
      : AstBase(NodeKind::PrototypeAST) {
    assert(functionName);
    this->functionName = sammine_util::QualifiedName::local(functionName->lexeme);
    this->join_location(functionName);

    this->parameterVectors = std::move(parameterVectors);

    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      assert(this->parameterVectors[i]);
    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      this->join_location(this->parameterVectors[i]->get_location());
  }

  bool returnsUnit() const { return return_type_expr == nullptr; }

  AST_NODE_METHODS("PrototypeAST", NodeKind::PrototypeAST)
};

//! \brief A Function Definition that has the prototype and definition in terms
//! of a block
class ExternAST : public DefinitionAST {
public:
  std::unique_ptr<PrototypeAST> Prototype;
  bool is_exposed = false;

  ExternAST(std::unique_ptr<PrototypeAST> Prototype)
      : DefinitionAST(NodeKind::ExternAST), Prototype(std::move(Prototype)) {
    this->join_location(this->Prototype.get());
  }
  AST_NODE_METHODS("ExternAST", NodeKind::ExternAST)
};
class ExprAST : public AstBase, public Printable {
public:
  bool is_statement = true;
  ExprAST(NodeKind kind) : AstBase(kind) {}
  ~ExprAST() = default;
  static bool classof(const AstBase *node) {
    return node->getKind() >= NodeKind::FirstExpr &&
           node->getKind() <= NodeKind::LastExpr;
  }
};

//! \brief An AST to simulate a { } code block
//!
//!
class BlockAST : public AstBase, public Printable {

public:
  BlockAST() : AstBase(NodeKind::BlockAST) {}
  std::vector<std::unique_ptr<ExprAST>> Statements;
  AST_NODE_METHODS("BlockAST", NodeKind::BlockAST)
};

class FuncDefAST : public DefinitionAST {
public:
  ~FuncDefAST() = default;
  std::unique_ptr<PrototypeAST> Prototype;
  std::unique_ptr<BlockAST> Block;
  bool is_exported = false;

  FuncDefAST(std::unique_ptr<PrototypeAST> Prototype,
             std::unique_ptr<BlockAST> Block)
      : DefinitionAST(NodeKind::FuncDefAST), Prototype(std::move(Prototype)),
        Block(std::move(Block)) {
    this->join_location(this->Prototype.get())
        ->join_location(this->Block.get());
  }

  std::string getFunctionName() const { return Prototype->functionName.mangled(); }

  bool returnsUnit() const { return Prototype->returnsUnit(); }

  AST_NODE_METHODS("FuncDefAST", NodeKind::FuncDefAST)
};

// struct id { typed_var }
class StructDefAST : public DefinitionAST {
public:
  sammine_util::QualifiedName struct_name;
  std::vector<std::unique_ptr<TypedVarAST>> struct_members;
  bool is_exported = false;

  explicit StructDefAST(std::shared_ptr<Token> struct_id,
                        decltype(struct_members) struct_members)
      : DefinitionAST(NodeKind::StructDefAST),
        struct_members(std::move(struct_members)) {
    if (struct_id)
      struct_name = sammine_util::QualifiedName::local(struct_id->lexeme);

    this->join_location(struct_id);
    for (auto &m : struct_members)
      this->join_location(m.get());
  }
  AST_NODE_METHODS("StructDefAST", NodeKind::StructDefAST)
};

// enum Name = Variant1(Type) | Variant2 | Variant3(Type, Type);
struct EnumVariantDef {
  std::string name;
  std::vector<std::unique_ptr<TypeExprAST>> payload_types;
  std::optional<int64_t> discriminant_value;
  sammine_util::Location location;
};

class EnumDefAST : public DefinitionAST {
public:
  sammine_util::QualifiedName enum_name;
  std::vector<EnumVariantDef> variants;
  std::vector<std::string> type_params;
  bool is_integer_backed = false;
  bool is_exported = false;
  std::optional<std::string> backing_type_name;

  explicit EnumDefAST(std::shared_ptr<Token> enum_id,
                      std::vector<EnumVariantDef> variants)
      : DefinitionAST(NodeKind::EnumDefAST), variants(std::move(variants)) {
    if (enum_id)
      enum_name = sammine_util::QualifiedName::local(enum_id->lexeme);
    this->join_location(enum_id);
    for (auto &v : this->variants)
      this->join_location(v.location);
  }
  AST_NODE_METHODS("EnumDefAST", NodeKind::EnumDefAST)
};

// type Name = ExistingType;
class TypeAliasDefAST : public DefinitionAST {
public:
  sammine_util::QualifiedName alias_name;
  std::unique_ptr<TypeExprAST> type_expr;
  bool is_exported = false;

  explicit TypeAliasDefAST(std::shared_ptr<Token> name_tok,
                           std::unique_ptr<TypeExprAST> type_expr)
      : DefinitionAST(NodeKind::TypeAliasDefAST),
        type_expr(std::move(type_expr)) {
    if (name_tok)
      alias_name = sammine_util::QualifiedName::local(name_tok->lexeme);
    this->join_location(name_tok);
    if (this->type_expr)
      this->join_location(this->type_expr->location);
  }
  AST_NODE_METHODS("TypeAliasDefAST", NodeKind::TypeAliasDefAST)
};

//! \brief A variable definition: "var x = expression;" or "let (a, b) = expr;"
class VarDefAST : public ExprAST {
public:
  bool is_mutable = false;
  bool is_tuple_destructure = false;
  std::unique_ptr<TypedVarAST> TypedVar;                      // single var (existing)
  std::vector<std::unique_ptr<TypedVarAST>> destructure_vars; // tuple (new)
  std::unique_ptr<ExprAST> Expression;

  explicit VarDefAST(std::shared_ptr<Token> let,
                     std::unique_ptr<TypedVarAST> TypedVar,
                     std::unique_ptr<ExprAST> Expression,
                     bool is_mutable = false)
      : ExprAST(NodeKind::VarDefAST), is_mutable(is_mutable),
        TypedVar(std::move(TypedVar)), Expression(std::move(Expression)) {

    this->join_location(let)
        ->join_location(this->TypedVar.get())
        ->join_location(this->Expression.get());
  };

  // Destructuring constructor: let (a, b) = expr;
  explicit VarDefAST(std::shared_ptr<Token> let,
                     std::vector<std::unique_ptr<TypedVarAST>> destructure_vars,
                     std::unique_ptr<ExprAST> Expression,
                     bool is_mutable = false)
      : ExprAST(NodeKind::VarDefAST), is_mutable(is_mutable),
        is_tuple_destructure(true),
        destructure_vars(std::move(destructure_vars)),
        Expression(std::move(Expression)) {
    this->join_location(let);
    for (auto &v : this->destructure_vars)
      this->join_location(v.get());
    this->join_location(this->Expression.get());
  };

  AST_NODE_METHODS("VarDefAST", NodeKind::VarDefAST)
};
class NumberExprAST : public ExprAST {
public:
  std::string number;

  explicit NumberExprAST(std::shared_ptr<Token> t)
      : ExprAST(NodeKind::NumberExprAST) {
    assert(t);
    join_location(t);
    number = t->lexeme;
  }
  AST_NODE_METHODS("NumberExprAST", NodeKind::NumberExprAST)
};
class StringExprAST : public ExprAST {
public:
  std::string string_content;

  explicit StringExprAST(std::shared_ptr<Token> t)
      : ExprAST(NodeKind::StringExprAST) {
    assert(t);
    join_location(t);
    string_content = t->lexeme;
  }
  AST_NODE_METHODS("StringExprAST", NodeKind::StringExprAST)
};

class BoolExprAST : public ExprAST {
public:
  bool b;
  BoolExprAST(bool b, sammine_util::Location loc)
      : ExprAST(NodeKind::BoolExprAST), b(b) {
    this->location = loc;
  }
  AST_NODE_METHODS("BoolExprAST", NodeKind::BoolExprAST)
};
class CharExprAST : public ExprAST {
public:
  char value;
  CharExprAST(char value, sammine_util::Location loc)
      : ExprAST(NodeKind::CharExprAST), value(value) {
    this->location = loc;
  }
  AST_NODE_METHODS("CharExprAST", NodeKind::CharExprAST)
};
class BinaryExprAST : public ExprAST {
public:
  std::shared_ptr<Token> Op;
  std::unique_ptr<ExprAST> LHS, RHS;
  BinaryExprAST(std::shared_ptr<Token> op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : ExprAST(NodeKind::BinaryExprAST), Op(op), LHS(std::move(LHS)),
        RHS(std::move(RHS)) {
    this->join_location(this->Op)
        ->join_location(this->LHS.get())
        ->join_location(this->RHS.get());
  }

  AST_NODE_METHODS("BinaryExprAST", NodeKind::BinaryExprAST)
};
class ReturnExprAST : public ExprAST {

public:
  bool is_implicit;
  std::unique_ptr<ExprAST> return_expr;
  ReturnExprAST(std::shared_ptr<Token> return_tok,
                std::unique_ptr<ExprAST> return_expr)
      : ExprAST(NodeKind::ReturnExprAST), is_implicit(false),
        return_expr(std::move(return_expr)) {
    if (this->return_expr == nullptr) {
      this->join_location(return_tok);
    } else if (return_tok == nullptr) {
      this->join_location(this->return_expr.get());
    } else if (return_tok && this->return_expr) {
      this->join_location(return_tok)->join_location(this->return_expr.get());
    }
  }

  ReturnExprAST(std::unique_ptr<ExprAST> return_expr)
      : ExprAST(NodeKind::ReturnExprAST), is_implicit(true),
        return_expr(std::move(return_expr)) {
    this->join_location(this->return_expr.get());
  }

  AST_NODE_METHODS("ReturnExprAST", NodeKind::ReturnExprAST)
};
class CallExprAST : public ExprAST {

public:
  sammine_util::QualifiedName functionName;
  std::vector<std::unique_ptr<AST::ExprAST>> arguments;
  std::vector<std::unique_ptr<TypeExprAST>> explicit_type_args;
  explicit CallExprAST(
      std::shared_ptr<Token> tok,
      std::vector<std::unique_ptr<AST::ExprAST>> arguments = {})
      : ExprAST(NodeKind::CallExprAST),
        functionName(sammine_util::QualifiedName::local(
            tok ? tok->lexeme : "")) {
    join_location(tok);
    for (auto &arg : arguments)
      if (arg)
        this->join_location(arg.get());
    this->arguments = std::move(arguments);
  }
  explicit CallExprAST(
      sammine_util::QualifiedName qn, sammine_util::Location loc,
      std::vector<std::unique_ptr<AST::ExprAST>> arguments = {})
      : ExprAST(NodeKind::CallExprAST), functionName(std::move(qn)) {
    this->location = loc;
    for (auto &arg : arguments)
      if (arg)
        this->join_location(arg.get());
    this->arguments = std::move(arguments);
  }

  AST_NODE_METHODS("CallExprAST", NodeKind::CallExprAST)
};

class UnitExprAST : public ExprAST {

public:
  bool is_implicit;

  explicit UnitExprAST(std::shared_ptr<Token> left_paren,
                       std::shared_ptr<Token> right_paren)
      : ExprAST(NodeKind::UnitExprAST), is_implicit(false) {
    assert(left_paren);
    assert(right_paren);
    this->join_location(left_paren)->join_location(right_paren);
  };
  explicit UnitExprAST() : ExprAST(NodeKind::UnitExprAST), is_implicit(true) {}

  AST_NODE_METHODS("UnitExprAST", NodeKind::UnitExprAST)
};
class IfExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> bool_expr;
  std::unique_ptr<BlockAST> thenBlockAST, elseBlockAST;
  explicit IfExprAST(std::unique_ptr<ExprAST> bool_expr,
                     std::unique_ptr<BlockAST> thenBlockAST,
                     std::unique_ptr<BlockAST> elseBlockAST)
      : ExprAST(NodeKind::IfExprAST), bool_expr(std::move(bool_expr)),
        thenBlockAST(std::move(thenBlockAST)),
        elseBlockAST(std::move(elseBlockAST)) {
    this->join_location(this->bool_expr.get())
        ->join_location(this->thenBlockAST.get())
        ->join_location(this->elseBlockAST.get());
  }

  AST_NODE_METHODS("IfExprAST", NodeKind::IfExprAST)
};
class VariableExprAST : public ExprAST {
public:
  std::string variableName;
  VariableExprAST(std::shared_ptr<Token> var)
      : ExprAST(NodeKind::VariableExprAST) {
    join_location(var);
    if (var)
      variableName = var->lexeme;
  };

  AST_NODE_METHODS("VariableExprAST", NodeKind::VariableExprAST)
};
class DerefExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit DerefExprAST(std::shared_ptr<Token> star_tok,
                        std::unique_ptr<ExprAST> operand)
      : ExprAST(NodeKind::DerefExprAST), operand(std::move(operand)) {
    this->join_location(star_tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("DerefExprAST", NodeKind::DerefExprAST)
};

class AddrOfExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit AddrOfExprAST(std::shared_ptr<Token> amp_tok,
                         std::unique_ptr<ExprAST> operand)
      : ExprAST(NodeKind::AddrOfExprAST), operand(std::move(operand)) {
    this->join_location(amp_tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("AddrOfExprAST", NodeKind::AddrOfExprAST)
};
class AllocExprAST : public ExprAST {
public:
  std::unique_ptr<TypeExprAST> type_arg; // the T in alloc<T>(count)
  std::unique_ptr<ExprAST> operand;      // the count expression
  explicit AllocExprAST(std::shared_ptr<Token> tok,
                        std::unique_ptr<TypeExprAST> type_arg,
                        std::unique_ptr<ExprAST> operand)
      : ExprAST(NodeKind::AllocExprAST), type_arg(std::move(type_arg)),
        operand(std::move(operand)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("AllocExprAST", NodeKind::AllocExprAST)
};

class FreeExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit FreeExprAST(std::shared_ptr<Token> tok,
                       std::unique_ptr<ExprAST> operand)
      : ExprAST(NodeKind::FreeExprAST), operand(std::move(operand)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("FreeExprAST", NodeKind::FreeExprAST)
};
class ArrayLiteralExprAST : public ExprAST {
public:
  std::vector<std::unique_ptr<ExprAST>> elements;
  explicit ArrayLiteralExprAST(std::vector<std::unique_ptr<ExprAST>> elements)
      : ExprAST(NodeKind::ArrayLiteralExprAST),
        elements(std::move(elements)) {
    for (auto &e : this->elements)
      if (e)
        this->join_location(e.get());
  }
  AST_NODE_METHODS("ArrayLiteralExprAST", NodeKind::ArrayLiteralExprAST)
};

class IndexExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> array_expr;
  std::unique_ptr<ExprAST> index_expr;
  explicit IndexExprAST(std::unique_ptr<ExprAST> array_expr,
                        std::unique_ptr<ExprAST> index_expr)
      : ExprAST(NodeKind::IndexExprAST), array_expr(std::move(array_expr)),
        index_expr(std::move(index_expr)) {
    this->join_location(this->array_expr.get())
        ->join_location(this->index_expr.get());
  }
  AST_NODE_METHODS("IndexExprAST", NodeKind::IndexExprAST)
};

class LenExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit LenExprAST(std::shared_ptr<Token> tok,
                      std::unique_ptr<ExprAST> operand)
      : ExprAST(NodeKind::LenExprAST), operand(std::move(operand)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("LenExprAST", NodeKind::LenExprAST)
};

class UnaryNegExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit UnaryNegExprAST(std::shared_ptr<Token> op_tok,
                           std::unique_ptr<ExprAST> operand)
      : ExprAST(NodeKind::UnaryNegExprAST), operand(std::move(operand)) {
    this->join_location(op_tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("UnaryNegExprAST", NodeKind::UnaryNegExprAST)
};

class StructLiteralExprAST : public ExprAST {
public:
  sammine_util::QualifiedName struct_name;
  std::vector<std::string> field_names;
  std::vector<std::unique_ptr<ExprAST>> field_values;
  explicit StructLiteralExprAST(
      std::shared_ptr<Token> name_tok,
      std::vector<std::string> field_names,
      std::vector<std::unique_ptr<ExprAST>> field_values)
      : ExprAST(NodeKind::StructLiteralExprAST),
        struct_name(sammine_util::QualifiedName::local(
            name_tok ? name_tok->lexeme : "")),
        field_names(std::move(field_names)),
        field_values(std::move(field_values)) {
    this->join_location(name_tok);
    for (auto &v : this->field_values)
      if (v)
        this->join_location(v.get());
  }
  explicit StructLiteralExprAST(
      sammine_util::QualifiedName qn, sammine_util::Location loc,
      std::vector<std::string> field_names,
      std::vector<std::unique_ptr<ExprAST>> field_values)
      : ExprAST(NodeKind::StructLiteralExprAST),
        struct_name(std::move(qn)),
        field_names(std::move(field_names)),
        field_values(std::move(field_values)) {
    this->location = loc;
    for (auto &v : this->field_values)
      if (v)
        this->join_location(v.get());
  }
  AST_NODE_METHODS("StructLiteralExprAST", NodeKind::StructLiteralExprAST)
};

class FieldAccessExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> object_expr;
  std::string field_name;
  explicit FieldAccessExprAST(std::unique_ptr<ExprAST> object_expr,
                              std::shared_ptr<Token> field_tok)
      : ExprAST(NodeKind::FieldAccessExprAST),
        object_expr(std::move(object_expr)) {
    this->join_location(this->object_expr.get());
    this->join_location(field_tok);
    if (field_tok)
      this->field_name = field_tok->lexeme;
  }
  AST_NODE_METHODS("FieldAccessExprAST", NodeKind::FieldAccessExprAST)
};

struct CasePattern {
  sammine_util::QualifiedName variant_name =
      sammine_util::QualifiedName::local("");
  std::vector<std::string> bindings;
  bool is_wildcard = false;
  sammine_util::Location location;
  size_t variant_index = 0;
};

struct CaseArm {
  CasePattern pattern;
  std::unique_ptr<BlockAST> body;
};

class CaseExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> scrutinee;
  std::vector<CaseArm> arms;

  explicit CaseExprAST(std::shared_ptr<Token> case_tok,
                       std::unique_ptr<ExprAST> scrutinee,
                       std::vector<CaseArm> arms)
      : ExprAST(NodeKind::CaseExprAST), scrutinee(std::move(scrutinee)),
        arms(std::move(arms)) {
    this->join_location(case_tok)->join_location(this->scrutinee.get());
    for (auto &arm : this->arms) {
      if (arm.body)
        this->join_location(arm.body.get());
    }
  }

  AST_NODE_METHODS("CaseExprAST", NodeKind::CaseExprAST)
};

class WhileExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> condition;
  std::unique_ptr<BlockAST> body;
  explicit WhileExprAST(std::unique_ptr<ExprAST> condition,
                        std::unique_ptr<BlockAST> body)
      : ExprAST(NodeKind::WhileExprAST), condition(std::move(condition)),
        body(std::move(body)) {
    this->join_location(this->condition.get())
        ->join_location(this->body.get());
  }
  AST_NODE_METHODS("WhileExprAST", NodeKind::WhileExprAST)
};

class TupleLiteralExprAST : public ExprAST {
public:
  std::vector<std::unique_ptr<ExprAST>> elements;
  explicit TupleLiteralExprAST(std::vector<std::unique_ptr<ExprAST>> elements)
      : ExprAST(NodeKind::TupleLiteralExprAST),
        elements(std::move(elements)) {
    for (auto &e : this->elements)
      if (e)
        this->join_location(e.get());
  }
  AST_NODE_METHODS("TupleLiteralExprAST", NodeKind::TupleLiteralExprAST)
};

class TypeClassDeclAST : public DefinitionAST {
public:
  std::string class_name;
  std::string type_param;
  std::vector<std::unique_ptr<PrototypeAST>> methods;
  explicit TypeClassDeclAST(std::shared_ptr<Token> tok, std::string name,
                            std::string param,
                            std::vector<std::unique_ptr<PrototypeAST>> methods)
      : DefinitionAST(NodeKind::TypeClassDeclAST),
        class_name(std::move(name)), type_param(std::move(param)),
        methods(std::move(methods)) {
    this->join_location(tok);
  }
  AST_NODE_METHODS("TypeClassDeclAST", NodeKind::TypeClassDeclAST)
};

class TypeClassInstanceAST : public DefinitionAST {
public:
  std::string class_name;
  std::unique_ptr<TypeExprAST> concrete_type_expr;
  std::vector<std::unique_ptr<FuncDefAST>> methods;
  explicit TypeClassInstanceAST(
      std::shared_ptr<Token> tok, std::string class_name,
      std::unique_ptr<TypeExprAST> type_expr,
      std::vector<std::unique_ptr<FuncDefAST>> methods)
      : DefinitionAST(NodeKind::TypeClassInstanceAST),
        class_name(std::move(class_name)),
        concrete_type_expr(std::move(type_expr)),
        methods(std::move(methods)) {
    this->join_location(tok);
  }
  AST_NODE_METHODS("TypeClassInstanceAST", NodeKind::TypeClassInstanceAST)
};

} // namespace AST
} // namespace sammine_lang

#undef AST_NODE_METHODS
