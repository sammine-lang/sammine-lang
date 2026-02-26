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
#include <unordered_map>
#include <vector>

//! \file Ast.h
//! \brief Defined the AST Node classes (ProgramAST, StructDefAST, FuncDefAST)
//! and a visitor interface for traversing the AST

// clang-format off
#define AST_NODE_METHODS(tree_name)                                            \
  std::string getTreeName() const override { return tree_name; }               \
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

class TypeExprAST {
public:
  sammine_util::Location location;
  virtual ~TypeExprAST() = default;
  virtual std::string to_string() const = 0;
};

class SimpleTypeExprAST : public TypeExprAST {
public:
  sammine_util::QualifiedName name;
  explicit SimpleTypeExprAST(std::shared_ptr<Token> tok)
      : name(sammine_util::QualifiedName::local(tok ? tok->lexeme : "")) {
    if (tok)
      location = tok->get_location();
  }
  explicit SimpleTypeExprAST(sammine_util::QualifiedName qn,
                             sammine_util::Location loc)
      : name(std::move(qn)) {
    location = loc;
  }
  std::string to_string() const override { return name.display(); }
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

struct ImportDecl {
  std::string module_name;
  std::string alias;
  sammine_util::Location location;
};

class ProgramAST : public AstBase, public Printable {
public:
  std::vector<ImportDecl> imports;
  std::vector<std::unique_ptr<DefinitionAST>> DefinitionVec;
  AST_NODE_METHODS("ProgramAST")
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
  AST_NODE_METHODS("TypedVarAST")
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
      : return_type_expr(std::move(return_type_expr)) {
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
      std::vector<std::unique_ptr<AST::TypedVarAST>> parameterVectors) {
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

  AST_NODE_METHODS("PrototypeAST")
};

//! \brief A Function Definition that has the prototype and definition in terms
//! of a block
class ExternAST : public DefinitionAST {
public:
  std::unique_ptr<PrototypeAST> Prototype;
  bool is_exposed = false;

  ExternAST(std::unique_ptr<PrototypeAST> Prototype)
      : Prototype(std::move(Prototype)) {
    this->join_location(this->Prototype.get());
  }
  AST_NODE_METHODS("ExternAST")
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
  AST_NODE_METHODS("BlockAST")
};

class FuncDefAST : public DefinitionAST {
public:
  ~FuncDefAST() = default;
  std::unique_ptr<PrototypeAST> Prototype;
  std::unique_ptr<BlockAST> Block;
  bool is_exported = false;

  FuncDefAST(std::unique_ptr<PrototypeAST> Prototype,
             std::unique_ptr<BlockAST> Block)
      : Prototype(std::move(Prototype)), Block(std::move(Block)) {
    this->join_location(this->Prototype.get())
        ->join_location(this->Block.get());
  }

  std::string getFunctionName() const { return Prototype->functionName.mangled(); }

  bool returnsUnit() const { return Prototype->returnsUnit(); }

  AST_NODE_METHODS("FuncDefAST")
};

// struct id { typed_var }
class StructDefAST : public DefinitionAST {
public:
  sammine_util::QualifiedName struct_name;
  std::vector<std::unique_ptr<TypedVarAST>> struct_members;
  bool is_exported = false;

  explicit StructDefAST(std::shared_ptr<Token> struct_id,
                        decltype(struct_members) struct_members)
      : struct_members(std::move(struct_members)) {
    if (struct_id)
      struct_name = sammine_util::QualifiedName::local(struct_id->lexeme);

    this->join_location(struct_id);
    for (auto &m : struct_members)
      this->join_location(m.get());
  }
  AST_NODE_METHODS("StructDefAST")
};

// enum Name = Variant1(Type) | Variant2 | Variant3(Type, Type);
struct EnumVariantDef {
  std::string name;
  std::vector<std::unique_ptr<TypeExprAST>> payload_types;
  sammine_util::Location location;
};

class EnumDefAST : public DefinitionAST {
public:
  sammine_util::QualifiedName enum_name;
  std::vector<EnumVariantDef> variants;
  std::vector<std::string> type_params;
  bool is_exported = false;

  explicit EnumDefAST(std::shared_ptr<Token> enum_id,
                      std::vector<EnumVariantDef> variants)
      : variants(std::move(variants)) {
    if (enum_id)
      enum_name = sammine_util::QualifiedName::local(enum_id->lexeme);
    this->join_location(enum_id);
    for (auto &v : this->variants)
      this->join_location(v.location);
  }
  AST_NODE_METHODS("EnumDefAST")
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

  AST_NODE_METHODS("VarDefAST")
};
class NumberExprAST : public ExprAST {
public:
  std::string number;

  explicit NumberExprAST(std::shared_ptr<Token> t) {
    assert(t);
    join_location(t);
    number = t->lexeme;
  }
  AST_NODE_METHODS("NumberExprAST")
};
class StringExprAST : public ExprAST {
public:
  std::string string_content;

  explicit StringExprAST(std::shared_ptr<Token> t) {
    assert(t);
    join_location(t);
    string_content = t->lexeme;
  }
  AST_NODE_METHODS("StringExprAST")
};

class BoolExprAST : public ExprAST {
public:
  bool b;
  BoolExprAST(bool b, sammine_util::Location loc) : b(b) {
    this->location = loc;
  }
  AST_NODE_METHODS("BoolExprAST")
};
class CharExprAST : public ExprAST {
public:
  char value;
  CharExprAST(char value, sammine_util::Location loc) : value(value) {
    this->location = loc;
  }
  AST_NODE_METHODS("CharExprAST")
};
class BinaryExprAST : public ExprAST {
public:
  std::shared_ptr<Token> Op;
  std::unique_ptr<ExprAST> LHS, RHS;
  std::string resolved_op_method; // Set by type checker for typeclass operator dispatch
  BinaryExprAST(std::shared_ptr<Token> op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {
    this->join_location(this->Op)
        ->join_location(this->LHS.get())
        ->join_location(this->RHS.get());
  }

  AST_NODE_METHODS("BinaryExprAST")
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

  AST_NODE_METHODS("ReturnExprAST")
};
class CallExprAST : public ExprAST {

public:
  sammine_util::QualifiedName functionName;
  std::vector<std::unique_ptr<AST::ExprAST>> arguments;
  std::optional<Type> callee_func_type;
  bool is_partial = false;
  std::optional<std::string> resolved_generic_name;
  std::unordered_map<std::string, Type> type_bindings;
  std::vector<std::unique_ptr<TypeExprAST>> explicit_type_args;
  bool is_typeclass_call = false;
  bool is_enum_constructor = false;
  size_t enum_variant_index = 0;
  explicit CallExprAST(
      std::shared_ptr<Token> tok,
      std::vector<std::unique_ptr<AST::ExprAST>> arguments = {})
      : functionName(sammine_util::QualifiedName::local(
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
      : functionName(std::move(qn)) {
    this->location = loc;
    for (auto &arg : arguments)
      if (arg)
        this->join_location(arg.get());
    this->arguments = std::move(arguments);
  }

  AST_NODE_METHODS("CallExprAST")
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

  AST_NODE_METHODS("UnitExprAST")
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

  AST_NODE_METHODS("IfExprAST")
};
class VariableExprAST : public ExprAST {
public:
  std::string variableName;
  bool is_enum_unit_variant = false;
  size_t enum_variant_index = 0;
  VariableExprAST(std::shared_ptr<Token> var) {
    join_location(var);
    if (var)
      variableName = var->lexeme;
  };

  AST_NODE_METHODS("VariableExprAST")
};
class DerefExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit DerefExprAST(std::shared_ptr<Token> star_tok,
                        std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(star_tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("DerefExprAST")
};

class AddrOfExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit AddrOfExprAST(std::shared_ptr<Token> amp_tok,
                         std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(amp_tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("AddrOfExprAST")
};
class AllocExprAST : public ExprAST {
public:
  std::unique_ptr<TypeExprAST> type_arg; // the T in alloc<T>(count)
  std::unique_ptr<ExprAST> operand;      // the count expression
  explicit AllocExprAST(std::shared_ptr<Token> tok,
                        std::unique_ptr<TypeExprAST> type_arg,
                        std::unique_ptr<ExprAST> operand)
      : type_arg(std::move(type_arg)), operand(std::move(operand)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("AllocExprAST")
};

class FreeExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit FreeExprAST(std::shared_ptr<Token> tok,
                       std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("FreeExprAST")
};
class ArrayLiteralExprAST : public ExprAST {
public:
  std::vector<std::unique_ptr<ExprAST>> elements;
  explicit ArrayLiteralExprAST(std::vector<std::unique_ptr<ExprAST>> elements)
      : elements(std::move(elements)) {
    for (auto &e : this->elements)
      if (e)
        this->join_location(e.get());
  }
  AST_NODE_METHODS("ArrayLiteralExprAST")
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
  AST_NODE_METHODS("IndexExprAST")
};

class LenExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit LenExprAST(std::shared_ptr<Token> tok,
                      std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("LenExprAST")
};

class UnaryNegExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> operand;
  explicit UnaryNegExprAST(std::shared_ptr<Token> op_tok,
                           std::unique_ptr<ExprAST> operand)
      : operand(std::move(operand)) {
    this->join_location(op_tok)->join_location(this->operand.get());
  }
  AST_NODE_METHODS("UnaryNegExprAST")
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
      : struct_name(sammine_util::QualifiedName::local(
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
      : struct_name(std::move(qn)),
        field_names(std::move(field_names)),
        field_values(std::move(field_values)) {
    this->location = loc;
    for (auto &v : this->field_values)
      if (v)
        this->join_location(v.get());
  }
  AST_NODE_METHODS("StructLiteralExprAST")
};

class FieldAccessExprAST : public ExprAST {
public:
  std::unique_ptr<ExprAST> object_expr;
  std::string field_name;
  explicit FieldAccessExprAST(std::unique_ptr<ExprAST> object_expr,
                              std::shared_ptr<Token> field_tok)
      : object_expr(std::move(object_expr)) {
    this->join_location(this->object_expr.get());
    this->join_location(field_tok);
    if (field_tok)
      this->field_name = field_tok->lexeme;
  }
  AST_NODE_METHODS("FieldAccessExprAST")
};

class TypeClassDeclAST : public DefinitionAST {
public:
  std::string class_name;
  std::string type_param;
  std::vector<std::unique_ptr<PrototypeAST>> methods;
  explicit TypeClassDeclAST(std::shared_ptr<Token> tok, std::string name,
                            std::string param,
                            std::vector<std::unique_ptr<PrototypeAST>> methods)
      : class_name(std::move(name)), type_param(std::move(param)),
        methods(std::move(methods)) {
    this->join_location(tok);
  }
  AST_NODE_METHODS("TypeClassDeclAST")
};

class TypeClassInstanceAST : public DefinitionAST {
public:
  std::string class_name;
  std::unique_ptr<TypeExprAST> concrete_type_expr;
  Type concrete_type = Type::NonExistent();
  std::vector<std::unique_ptr<FuncDefAST>> methods;
  explicit TypeClassInstanceAST(
      std::shared_ptr<Token> tok, std::string class_name,
      std::unique_ptr<TypeExprAST> type_expr,
      std::vector<std::unique_ptr<FuncDefAST>> methods)
      : class_name(std::move(class_name)),
        concrete_type_expr(std::move(type_expr)),
        methods(std::move(methods)) {
    this->join_location(tok);
  }
  AST_NODE_METHODS("TypeClassInstanceAST")
};

} // namespace AST
} // namespace sammine_lang

#undef AST_NODE_METHODS
