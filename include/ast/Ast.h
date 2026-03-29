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

/// Generates boilerplate for each AST node: getTreeName (debug printing),
/// classof (LLVM RTTI), accept_vis/walk_with_preorder/postorder (visitor
/// dispatch), and accept_synthesis (type checker dispatch).
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

/// Discriminator for the TypeExprAST hierarchy.
/// NOTE: TypeExprAST is NOT part of the visitor pattern — resolved via
/// dynamic_cast in the type checker (BiTypeChecker::resolve_type_expr).
enum class ParseKind {
  Simple,   // e.g. i32, bool, MyStruct
  Pointer,  // ptr<T> or 'ptr<T>
  Array,    // [T; N]
  Function, // (T, U) -> V
  Generic,  // Box<T>, Option<i32>
  Tuple,    // (T, U)
};

/// Base class for parsed type annotations. Produces a Type during type
/// checking.
class TypeExprAST {
  ParseKind kind;

public:
  ParseKind getKind() const { return kind; }
  sammine_util::Location location;

  /// When set, resolve_type_expr returns this directly instead of resolving
  /// the AST node. Used by the monomorphizer to attach pre-resolved types
  /// (e.g. when substituting a type param with a compound type like a tuple).
  std::optional<Type> resolved_type;

  TypeExprAST(ParseKind kind_) : kind(kind_) {}
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
  std::string to_string() const override { return name.mangled(); }
};

class PointerTypeExprAST : public TypeExprAST {
public:
  std::unique_ptr<TypeExprAST> pointee;
  bool is_linear = false;
  explicit PointerTypeExprAST(std::unique_ptr<TypeExprAST> pointee_,
                              bool is_linear_ = false)
      : TypeExprAST(ParseKind::Pointer), pointee(std::move(pointee_)),
        is_linear(is_linear_) {}
  static bool classof(const TypeExprAST *node) {
    return node->getKind() == ParseKind::Pointer;
  }
  std::string to_string() const override {
    return (is_linear ? "'" : "") + std::string("ptr<") + pointee->to_string() +
           ">";
  }
};

class ArrayTypeExprAST : public TypeExprAST {
public:
  std::unique_ptr<TypeExprAST> element;
  size_t size;
  ArrayTypeExprAST(std::unique_ptr<TypeExprAST> element_, size_t size_)
      : TypeExprAST(ParseKind::Array), element(std::move(element_)), size(size_) {
  }
  static bool classof(const TypeExprAST *node) {
    return node->getKind() == ParseKind::Array;
  }
  std::string to_string() const override {
    return "[" + std::to_string(size) + "]" + element->to_string();
  }
};

class FunctionTypeExprAST : public TypeExprAST {
public:
  std::vector<std::unique_ptr<TypeExprAST>> paramTypes;
  std::unique_ptr<TypeExprAST> returnType;
  FunctionTypeExprAST(std::vector<std::unique_ptr<TypeExprAST>> paramTypes_,
                      std::unique_ptr<TypeExprAST> returnType_)
      : TypeExprAST(ParseKind::Function), paramTypes(std::move(paramTypes_)),
        returnType(std::move(returnType_)) {}
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
  GenericTypeExprAST(sammine_util::QualifiedName base_name_,
                     std::vector<std::unique_ptr<TypeExprAST>> type_args_,
                     sammine_util::Location loc)
      : TypeExprAST(ParseKind::Generic), base_name(std::move(base_name_)),
        type_args(std::move(type_args_)) {
    location = loc;
  }
  static bool classof(const TypeExprAST *node) {
    return node->getKind() == ParseKind::Generic;
  }
  std::string to_string() const override {
    std::string res = base_name.mangled() + "<";
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
  TupleTypeExprAST(std::vector<std::unique_ptr<TypeExprAST>> element_types_)
      : TypeExprAST(ParseKind::Tuple), element_types(std::move(element_types_)) {
  }
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
  bool is_exported = false;

  DefinitionAST(NodeKind kind_) : AstBase(kind_) {}
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
  std::unique_ptr<TypeExprAST> type_expr;

  explicit TypedVarAST(std::shared_ptr<Token> name_,
                       std::unique_ptr<TypeExprAST> type_expr_)
      : AstBase(NodeKind::TypedVarAST),
        type_expr(std::move(type_expr_)) {
    this->join_location(name_);
    if (name_)
      this->name = name_->lexeme;
    if (this->type_expr)
      this->join_location(this->type_expr->location);
  }
  explicit TypedVarAST(std::shared_ptr<Token> name_)
      : AstBase(NodeKind::TypedVarAST) {
    this->join_location(name_);
    if (name_)
      this->name = name_->lexeme;
  }
  std::string to_string() const;
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
  std::vector<std::string>
      type_params;         // populated during parsing, not type checking
  bool is_var_arg = false; // true for C vararg externs (e.g. printf)
  bool is_generic() const { return !type_params.empty(); }

  explicit PrototypeAST(
      sammine_util::QualifiedName functionName_, sammine_util::Location name_loc,
      std::unique_ptr<TypeExprAST> return_type_expr_,
      std::vector<std::unique_ptr<AST::TypedVarAST>> parameterVectors_)
      : AstBase(NodeKind::PrototypeAST),
        return_type_expr(std::move(return_type_expr_)) {
    this->functionName = std::move(functionName_);
    this->join_location(name_loc);
    if (this->return_type_expr)
      this->join_location(this->return_type_expr->location);

    this->parameterVectors = std::move(parameterVectors_);

    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      assert(this->parameterVectors[i]);
    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      this->join_location(this->parameterVectors[i]->get_location());
  }

  explicit PrototypeAST(
      sammine_util::QualifiedName functionName_, sammine_util::Location name_loc,
      std::vector<std::unique_ptr<AST::TypedVarAST>> parameterVectors_)
      : AstBase(NodeKind::PrototypeAST) {
    this->functionName = std::move(functionName_);
    this->join_location(name_loc);

    this->parameterVectors = std::move(parameterVectors_);

    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      assert(this->parameterVectors[i]);
    for (size_t i = 0; i < this->parameterVectors.size(); i++)
      this->join_location(this->parameterVectors[i]->get_location());
  }

  bool returnsUnit() const { return return_type_expr == nullptr; }

  std::string to_string() const;

  AST_NODE_METHODS("PrototypeAST", NodeKind::PrototypeAST)
};

//! \brief A Function Definition that has the prototype and definition in terms
//! of a block
/// C FFI declaration: `extern name(params) -> type;`
class ExternAST : public DefinitionAST {
public:
  std::unique_ptr<PrototypeAST> Prototype;

  ExternAST(std::unique_ptr<PrototypeAST> Prototype_)
      : DefinitionAST(NodeKind::ExternAST), Prototype(std::move(Prototype_)) {
    this->join_location(this->Prototype.get());
  }
  AST_NODE_METHODS("ExternAST", NodeKind::ExternAST)
};
/// Base class for all expression AST nodes. Expressions can appear as
/// statements (is_statement=true, value discarded) or as values.
class ExprAST : public AstBase, public Printable {
public:
  bool is_statement =
      true; // false when used as value (e.g. last expr in block)
  ExprAST(NodeKind kind_) : AstBase(kind_) {}
  ~ExprAST() = default;
  static bool classof(const AstBase *node) {
    return node->getKind() >= NodeKind::FirstExpr &&
           node->getKind() <= NodeKind::LastExpr;
  }
  virtual std::string to_string() const = 0;
};

//! \brief An AST to simulate a { } code block
//!
//!
class BlockAST : public AstBase, public Printable {

public:
  BlockAST() : AstBase(NodeKind::BlockAST) {}
  std::vector<std::unique_ptr<ExprAST>> Statements;
  std::string to_string() const;
  AST_NODE_METHODS("BlockAST", NodeKind::BlockAST)
};

class FuncDefAST : public DefinitionAST {
public:
  ~FuncDefAST() = default;
  std::unique_ptr<PrototypeAST> Prototype;
  std::unique_ptr<BlockAST> Block;

  FuncDefAST(std::unique_ptr<PrototypeAST> Prototype_,
             std::unique_ptr<BlockAST> Block_)
      : DefinitionAST(NodeKind::FuncDefAST), Prototype(std::move(Prototype_)),
        Block(std::move(Block_)) {
    this->join_location(this->Prototype.get())
        ->join_location(this->Block.get());
  }

  std::string getFunctionName() const {
    return Prototype->functionName.mangled();
  }

  bool returnsUnit() const { return Prototype->returnsUnit(); }

  std::string to_string() const;

  AST_NODE_METHODS("FuncDefAST", NodeKind::FuncDefAST)
};

// struct id { typed_var }
class StructDefAST : public DefinitionAST {
public:
  sammine_util::QualifiedName struct_name;
  std::vector<std::unique_ptr<TypedVarAST>> struct_members;
  std::vector<std::string> type_params;

  explicit StructDefAST(sammine_util::QualifiedName name,
                        sammine_util::Location name_loc,
                        decltype(struct_members) struct_members_)
      : DefinitionAST(NodeKind::StructDefAST), struct_name(std::move(name)),
        struct_members(std::move(struct_members_)) {
    this->join_location(name_loc);
    for (auto &m : this->struct_members)
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
  std::optional<std::string> backing_type_name;

  explicit EnumDefAST(sammine_util::QualifiedName name,
                      sammine_util::Location name_loc,
                      std::vector<EnumVariantDef> variants_)
      : DefinitionAST(NodeKind::EnumDefAST), enum_name(std::move(name)),
        variants(std::move(variants_)) {
    this->join_location(name_loc);
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

  explicit TypeAliasDefAST(std::shared_ptr<Token> name_tok,
                           std::unique_ptr<TypeExprAST> type_expr_)
      : DefinitionAST(NodeKind::TypeAliasDefAST),
        type_expr(std::move(type_expr_)) {
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
  bool is_tuple_destructure = false;
  std::unique_ptr<TypedVarAST> TypedVar; // single var (existing)
  std::vector<std::unique_ptr<TypedVarAST>> destructure_vars; // tuple (new)
  std::unique_ptr<ExprAST> Expression;

  explicit VarDefAST(std::shared_ptr<Token> let,
                     std::unique_ptr<TypedVarAST> TypedVar_,
                     std::unique_ptr<ExprAST> Expression_)
      : ExprAST(NodeKind::VarDefAST),
        TypedVar(std::move(TypedVar_)), Expression(std::move(Expression_)) {

    this->join_location(let)
        ->join_location(this->TypedVar.get())
        ->join_location(this->Expression.get());
  };

  // Destructuring constructor: let (a, b) = expr;
  explicit VarDefAST(std::shared_ptr<Token> let,
                     std::vector<std::unique_ptr<TypedVarAST>> destructure_vars_,
                     std::unique_ptr<ExprAST> Expression_)
      : ExprAST(NodeKind::VarDefAST),
        is_tuple_destructure(true),
        destructure_vars(std::move(destructure_vars_)),
        Expression(std::move(Expression_)) {
    this->join_location(let);
    for (auto &v : this->destructure_vars)
      this->join_location(v.get());
    this->join_location(this->Expression.get());
  };

  std::string to_string() const override;
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
  std::string to_string() const override;
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
  std::string to_string() const override;
  AST_NODE_METHODS("StringExprAST", NodeKind::StringExprAST)
};

class BoolExprAST : public ExprAST {
public:
  bool b;
  BoolExprAST(bool b_, sammine_util::Location loc)
      : ExprAST(NodeKind::BoolExprAST), b(b_) {
    this->location = loc;
  }
  std::string to_string() const override;
  AST_NODE_METHODS("BoolExprAST", NodeKind::BoolExprAST)
};
class CharExprAST : public ExprAST {
public:
  char value;
  CharExprAST(char value_, sammine_util::Location loc)
      : ExprAST(NodeKind::CharExprAST), value(value_) {
    this->location = loc;
  }
  std::string to_string() const override;
  AST_NODE_METHODS("CharExprAST", NodeKind::CharExprAST)
};
class BinaryExprAST : public ExprAST {
public:
  std::shared_ptr<Token> Op;
  std::unique_ptr<ExprAST> LHS, RHS;
  BinaryExprAST(std::shared_ptr<Token> op, std::unique_ptr<ExprAST> LHS_,
                std::unique_ptr<ExprAST> RHS_)
      : ExprAST(NodeKind::BinaryExprAST), Op(op), LHS(std::move(LHS_)),
        RHS(std::move(RHS_)) {
    this->join_location(this->Op)
        ->join_location(this->LHS.get())
        ->join_location(this->RHS.get());
  }

  std::string to_string() const override;
  AST_NODE_METHODS("BinaryExprAST", NodeKind::BinaryExprAST)
};
class ReturnStmtAST : public ExprAST {

public:
  bool is_implicit; // true when generated from last expression in a block (no
                    // `return` keyword)
  std::unique_ptr<ExprAST> return_expr;
  ReturnStmtAST(std::shared_ptr<Token> return_tok,
                std::unique_ptr<ExprAST> return_expr_)
      : ExprAST(NodeKind::ReturnStmtAST), is_implicit(false),
        return_expr(std::move(return_expr_)) {
    if (this->return_expr == nullptr) {
      this->join_location(return_tok);
    } else if (return_tok == nullptr) {
      this->join_location(this->return_expr.get());
    } else if (return_tok && this->return_expr) {
      this->join_location(return_tok)->join_location(this->return_expr.get());
    }
  }

  ReturnStmtAST(std::unique_ptr<ExprAST> return_expr_)
      : ExprAST(NodeKind::ReturnStmtAST), is_implicit(true),
        return_expr(std::move(return_expr_)) {
    this->join_location(this->return_expr.get());
  }

  std::string to_string() const override;
  AST_NODE_METHODS("ReturnStmtAST", NodeKind::ReturnStmtAST)
};
class CallExprAST : public ExprAST {

public:
  sammine_util::QualifiedName functionName;
  std::vector<std::unique_ptr<AST::ExprAST>> arguments;
  std::vector<std::unique_ptr<TypeExprAST>> explicit_type_args;
  explicit CallExprAST(
      std::shared_ptr<Token> tok,
      std::vector<std::unique_ptr<AST::ExprAST>> arguments_ = {})
      : ExprAST(NodeKind::CallExprAST),
        functionName(
            sammine_util::QualifiedName::local(tok ? tok->lexeme : "")) {
    join_location(tok);
    for (auto &arg : arguments_)
      if (arg)
        this->join_location(arg.get());
    this->arguments = std::move(arguments_);
  }
  explicit CallExprAST(
      sammine_util::QualifiedName qn, sammine_util::Location loc,
      std::vector<std::unique_ptr<AST::ExprAST>> arguments_ = {})
      : ExprAST(NodeKind::CallExprAST), functionName(std::move(qn)) {
    this->location = loc;
    for (auto &arg : arguments_)
      if (arg)
        this->join_location(arg.get());
    this->arguments = std::move(arguments_);
  }

  std::string to_string() const override;
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

  std::string to_string() const override;
  AST_NODE_METHODS("UnitExprAST", NodeKind::UnitExprAST)
};
class IfExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> bool_expr;
  std::unique_ptr<BlockAST> thenBlockAST, elseBlockAST;
  explicit IfExprAST(std::unique_ptr<ExprAST> bool_expr_,
                     std::unique_ptr<BlockAST> thenBlockAST_,
                     std::unique_ptr<BlockAST> elseBlockAST_)
      : ExprAST(NodeKind::IfExprAST), bool_expr(std::move(bool_expr_)),
        thenBlockAST(std::move(thenBlockAST_)),
        elseBlockAST(std::move(elseBlockAST_)) {
    this->join_location(this->bool_expr.get())
        ->join_location(this->thenBlockAST.get())
        ->join_location(this->elseBlockAST.get());
  }

  std::string to_string() const override;
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

  std::string to_string() const override;
  AST_NODE_METHODS("VariableExprAST", NodeKind::VariableExprAST)
};
class DerefExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit DerefExprAST(std::shared_ptr<Token> star_tok,
                        std::unique_ptr<ExprAST> operand_)
      : ExprAST(NodeKind::DerefExprAST), operand(std::move(operand_)) {
    this->join_location(star_tok)->join_location(this->operand.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("DerefExprAST", NodeKind::DerefExprAST)
};

class AddrOfExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit AddrOfExprAST(std::shared_ptr<Token> amp_tok,
                         std::unique_ptr<ExprAST> operand_)
      : ExprAST(NodeKind::AddrOfExprAST), operand(std::move(operand_)) {
    this->join_location(amp_tok)->join_location(this->operand.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("AddrOfExprAST", NodeKind::AddrOfExprAST)
};
class AllocExprAST : public ExprAST {
public:
  std::unique_ptr<TypeExprAST> type_arg; // the T in alloc<T>(count)
  std::unique_ptr<ExprAST> operand;      // the count expression
  explicit AllocExprAST(std::shared_ptr<Token> tok,
                        std::unique_ptr<TypeExprAST> type_arg_,
                        std::unique_ptr<ExprAST> operand_)
      : ExprAST(NodeKind::AllocExprAST), type_arg(std::move(type_arg_)),
        operand(std::move(operand_)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("AllocExprAST", NodeKind::AllocExprAST)
};

class FreeExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit FreeExprAST(std::shared_ptr<Token> tok,
                       std::unique_ptr<ExprAST> operand_)
      : ExprAST(NodeKind::FreeExprAST), operand(std::move(operand_)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("FreeExprAST", NodeKind::FreeExprAST)
};
class ArrayLiteralExprAST : public ExprAST {
public:
  std::vector<std::unique_ptr<ExprAST>> elements;
  explicit ArrayLiteralExprAST(std::vector<std::unique_ptr<ExprAST>> elements_)
      : ExprAST(NodeKind::ArrayLiteralExprAST), elements(std::move(elements_)) {
    for (auto &e : this->elements)
      this->join_location(e.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("ArrayLiteralExprAST", NodeKind::ArrayLiteralExprAST)
};

class RangeExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> start;
  std::unique_ptr<ExprAST> end;
  explicit RangeExprAST(std::unique_ptr<ExprAST> start_,
                        std::unique_ptr<ExprAST> end_)
      : ExprAST(NodeKind::RangeExprAST), start(std::move(start_)),
        end(std::move(end_)) {
    this->join_location(this->start.get())->join_location(this->end.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("RangeExprAST", NodeKind::RangeExprAST)
};

class IndexExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> array_expr;
  std::unique_ptr<ExprAST> index_expr;
  explicit IndexExprAST(std::unique_ptr<ExprAST> array_expr_,
                        std::unique_ptr<ExprAST> index_expr_)
      : ExprAST(NodeKind::IndexExprAST), array_expr(std::move(array_expr_)),
        index_expr(std::move(index_expr_)) {
    this->join_location(this->array_expr.get())
        ->join_location(this->index_expr.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("IndexExprAST", NodeKind::IndexExprAST)
};

class LenExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit LenExprAST(std::shared_ptr<Token> tok,
                      std::unique_ptr<ExprAST> operand_)
      : ExprAST(NodeKind::LenExprAST), operand(std::move(operand_)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("LenExprAST", NodeKind::LenExprAST)
};

class DimExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit DimExprAST(std::shared_ptr<Token> tok,
                      std::unique_ptr<ExprAST> operand_)
      : ExprAST(NodeKind::DimExprAST), operand(std::move(operand_)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("DimExprAST", NodeKind::DimExprAST)
};

class UnaryNegExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit UnaryNegExprAST(std::shared_ptr<Token> op_tok,
                           std::unique_ptr<ExprAST> operand_)
      : ExprAST(NodeKind::UnaryNegExprAST), operand(std::move(operand_)) {
    this->join_location(op_tok)->join_location(this->operand.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("UnaryNegExprAST", NodeKind::UnaryNegExprAST)
};

class StructLiteralExprAST : public ExprAST {
public:
  sammine_util::QualifiedName struct_name;
  std::vector<std::string> field_names;
  std::vector<std::unique_ptr<ExprAST>> field_values;
  std::vector<std::unique_ptr<TypeExprAST>> explicit_type_args;
  explicit StructLiteralExprAST(
      std::shared_ptr<Token> name_tok, std::vector<std::string> field_names_,
      std::vector<std::unique_ptr<ExprAST>> field_values_)
      : ExprAST(NodeKind::StructLiteralExprAST),
        struct_name(sammine_util::QualifiedName::local(
            name_tok ? name_tok->lexeme : "")),
        field_names(std::move(field_names_)),
        field_values(std::move(field_values_)) {
    this->join_location(name_tok);
    for (auto &v : this->field_values)
      if (v)
        this->join_location(v.get());
  }
  explicit StructLiteralExprAST(
      sammine_util::QualifiedName qn, sammine_util::Location loc,
      std::vector<std::string> field_names_,
      std::vector<std::unique_ptr<ExprAST>> field_values_)
      : ExprAST(NodeKind::StructLiteralExprAST), struct_name(std::move(qn)),
        field_names(std::move(field_names_)),
        field_values(std::move(field_values_)) {
    this->location = loc;
    for (auto &v : this->field_values)
      if (v)
        this->join_location(v.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("StructLiteralExprAST", NodeKind::StructLiteralExprAST)
};

class FieldAccessExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> object_expr;
  std::string field_name;
  explicit FieldAccessExprAST(std::unique_ptr<ExprAST> object_expr_,
                              std::shared_ptr<Token> field_tok)
      : ExprAST(NodeKind::FieldAccessExprAST),
        object_expr(std::move(object_expr_)) {
    this->join_location(this->object_expr.get());
    this->join_location(field_tok);
    if (field_tok)
      this->field_name = field_tok->lexeme;
  }
  std::string to_string() const override;
  AST_NODE_METHODS("FieldAccessExprAST", NodeKind::FieldAccessExprAST)
};

/// Pattern in a case arm: `Enum::Variant(x, y)`, `_` (wildcard), or a literal.
struct CasePattern {
  sammine_util::QualifiedName variant_name =
      sammine_util::QualifiedName::local("");
  std::vector<std::string>
      bindings;             // bound variables from payload destructuring
  bool is_wildcard = false; // true for `_` catch-all pattern
  sammine_util::Location location;
  size_t variant_index = 0; // resolved during type checking

  // Literal pattern fields (e.g., `0`, `42`, `true`, `'a'`, `-1`)
  enum class LiteralKind { None, Integer, Bool, Char };
  bool is_literal = false;
  std::string literal_value; // raw string: "42", "-42", "true", "a"
  LiteralKind literal_kind = LiteralKind::None; // set by parser
};

/// A single arm in a case expression: pattern => body block.
struct CaseArm {
  CasePattern pattern;
  std::unique_ptr<BlockAST> body;
};

class CaseExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> scrutinee;
  std::vector<CaseArm> arms;

  explicit CaseExprAST(std::shared_ptr<Token> case_tok,
                       std::unique_ptr<ExprAST> scrutinee_,
                       std::vector<CaseArm> arms_)
      : ExprAST(NodeKind::CaseExprAST), scrutinee(std::move(scrutinee_)),
        arms(std::move(arms_)) {
    this->join_location(case_tok)->join_location(this->scrutinee.get());
    for (auto &arm : this->arms) {
      if (arm.body)
        this->join_location(arm.body.get());
    }
  }

  std::string to_string() const override;
  AST_NODE_METHODS("CaseExprAST", NodeKind::CaseExprAST)
};

class WhileExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> condition;
  std::unique_ptr<BlockAST> body;
  explicit WhileExprAST(std::unique_ptr<ExprAST> condition_,
                        std::unique_ptr<BlockAST> body_)
      : ExprAST(NodeKind::WhileExprAST), condition(std::move(condition_)),
        body(std::move(body_)) {
    this->join_location(this->condition.get())->join_location(this->body.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("WhileExprAST", NodeKind::WhileExprAST)
};

class ForExprAST : public ExprAST {
public:
  std::string loop_var;
  std::unique_ptr<ExprAST> start;
  std::unique_ptr<ExprAST> end;
  std::unique_ptr<BlockAST> body;

  explicit ForExprAST(std::shared_ptr<Token> for_tok, std::string loop_var_,
                      std::unique_ptr<ExprAST> start_,
                      std::unique_ptr<ExprAST> end_,
                      std::unique_ptr<BlockAST> body_)
      : ExprAST(NodeKind::ForExprAST), loop_var(std::move(loop_var_)),
        start(std::move(start_)), end(std::move(end_)),
        body(std::move(body_)) {
    this->join_location(for_tok);
    if (this->end)
      this->join_location(this->end.get());
    if (this->body)
      this->join_location(this->body.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("ForExprAST", NodeKind::ForExprAST)
};

class TupleLiteralExprAST : public ExprAST {
public:
  std::vector<std::unique_ptr<ExprAST>> elements;
  explicit TupleLiteralExprAST(std::vector<std::unique_ptr<ExprAST>> elements_)
      : ExprAST(NodeKind::TupleLiteralExprAST), elements(std::move(elements_)) {
    for (auto &e : this->elements)
      this->join_location(e.get());
  }
  std::string to_string() const override;
  AST_NODE_METHODS("TupleLiteralExprAST", NodeKind::TupleLiteralExprAST)
};

/// Typeclass declaration: `typeclass Name<T> { method signatures }`.
class TypeClassDeclAST : public DefinitionAST {
public:
  std::string class_name;
  std::vector<std::string> type_params;
  std::vector<std::unique_ptr<PrototypeAST>> methods;
  explicit TypeClassDeclAST(std::shared_ptr<Token> tok, std::string name,
                            std::vector<std::string> params,
                            std::vector<std::unique_ptr<PrototypeAST>> methods_)
      : DefinitionAST(NodeKind::TypeClassDeclAST), class_name(std::move(name)),
        type_params(std::move(params)), methods(std::move(methods_)) {
    this->join_location(tok);
  }
  AST_NODE_METHODS("TypeClassDeclAST", NodeKind::TypeClassDeclAST)
};

/// Typeclass instance: `instance Name<ConcreteType> { method implementations
/// }`.
class TypeClassInstanceAST : public DefinitionAST {
public:
  std::string class_name;
  std::vector<std::unique_ptr<TypeExprAST>> concrete_type_exprs;
  std::vector<std::unique_ptr<FuncDefAST>> methods;
  explicit TypeClassInstanceAST(
      std::shared_ptr<Token> tok, std::string class_name_,
      std::vector<std::unique_ptr<TypeExprAST>> type_exprs,
      std::vector<std::unique_ptr<FuncDefAST>> methods_)
      : DefinitionAST(NodeKind::TypeClassInstanceAST),
        class_name(std::move(class_name_)),
        concrete_type_exprs(std::move(type_exprs)),
        methods(std::move(methods_)) {
    this->join_location(tok);
  }
  AST_NODE_METHODS("TypeClassInstanceAST", NodeKind::TypeClassInstanceAST)
};

/// Base class for kernel-only expressions (parallel to ExprAST for CPU code).
/// Kernel expressions don't participate in the CPU visitor pattern — stubs
/// provided here.
class KernelExprAST : public AstBase, public Printable {
public:
  KernelExprAST(NodeKind kind_) : AstBase(kind_) {}
  ~KernelExprAST() = default;
  static bool classof(const AstBase *node) {
    return node->getKind() >= NodeKind::FirstKernelExpr &&
           node->getKind() <= NodeKind::LastKernelExpr;
  }
  // Kernel exprs are not visited by CPU visitors — stubs only.
  std::string getTreeName() const override { return "KernelExprAST"; }
  void accept_vis(ASTVisitor *) override {}
  void walk_with_preorder(ASTVisitor *) override {}
  void walk_with_postorder(ASTVisitor *) override {}
  Type accept_synthesis(TypeCheckerVisitor *) override {
    return Type::NonExistent();
  }
};

/// Kernel integer literal expression.
class KernelNumberExprAST : public KernelExprAST {
public:
  std::string number;

  explicit KernelNumberExprAST(std::shared_ptr<Token> t)
      : KernelExprAST(NodeKind::KernelNumberExprAST) {
    assert(t);
    join_location(t);
    number = t->lexeme;
  }
  std::string getTreeName() const override { return "KernelNumberExprAST"; }
  static bool classof(const AstBase *node) {
    return node->getKind() == NodeKind::KernelNumberExprAST;
  }
};

/// Kernel map expression: map(array_param, (param: T) -> U { body })
/// The lambda reuses CPU AST nodes (PrototypeAST + BlockAST) for unified
/// front-end.
class KernelMapExprAST : public KernelExprAST {
public:
  std::string input_name;
  std::unique_ptr<PrototypeAST> lambda_proto;
  std::unique_ptr<BlockAST> lambda_body;

  KernelMapExprAST(std::shared_ptr<Token> map_tok, std::string input_name_,
                   std::unique_ptr<PrototypeAST> proto,
                   std::unique_ptr<BlockAST> body)
      : KernelExprAST(NodeKind::KernelMapExprAST),
        input_name(std::move(input_name_)), lambda_proto(std::move(proto)),
        lambda_body(std::move(body)) {
    join_location(map_tok);
    join_location(lambda_proto.get());
    join_location(lambda_body.get());
  }
  std::string getTreeName() const override { return "KernelMapExprAST"; }
  static bool classof(const AstBase *node) {
    return node->getKind() == NodeKind::KernelMapExprAST;
  }
};

/// Kernel reduce expression: reduce(array_param, op, identity)
/// The identity is a regular CPU ExprAST for unified front-end.
class KernelReduceExprAST : public KernelExprAST {
public:
  std::string input_name;
  std::shared_ptr<Token> op_tok;
  std::unique_ptr<ExprAST> identity;

  KernelReduceExprAST(std::shared_ptr<Token> reduce_tok, std::string input_name_,
                      std::shared_ptr<Token> op_tok_,
                      std::unique_ptr<ExprAST> identity_)
      : KernelExprAST(NodeKind::KernelReduceExprAST),
        input_name(std::move(input_name_)), op_tok(std::move(op_tok_)),
        identity(std::move(identity_)) {
    join_location(reduce_tok);
    join_location(this->op_tok);
    if (this->identity)
      join_location(this->identity.get());
  }
  std::string getTreeName() const override { return "KernelReduceExprAST"; }
  static bool classof(const AstBase *node) {
    return node->getKind() == NodeKind::KernelReduceExprAST;
  }
};

/// Kernel block: holds a vector of kernel-only expressions.
class KernelBlockAST {
public:
  std::vector<std::unique_ptr<KernelExprAST>> expressions;
  explicit KernelBlockAST(std::vector<std::unique_ptr<KernelExprAST>> exprs)
      : expressions(std::move(exprs)) {}
};

/// Kernel function definition: `kernel name(params) -> T { kernel_exprs }`.
class KernelDefAST : public DefinitionAST {
public:
  std::unique_ptr<PrototypeAST> Prototype;
  std::unique_ptr<KernelBlockAST> Body;

  KernelDefAST(std::unique_ptr<PrototypeAST> proto,
               std::unique_ptr<KernelBlockAST> body)
      : DefinitionAST(NodeKind::KernelDefAST), Prototype(std::move(proto)),
        Body(std::move(body)) {
    join_location(Prototype.get());
  }
  AST_NODE_METHODS("KernelDefAST", NodeKind::KernelDefAST)
};

} // namespace AST
} // namespace sammine_lang

#undef AST_NODE_METHODS
