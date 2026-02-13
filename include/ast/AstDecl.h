#pragma once
#include <string>
//! \file AstDecl.h
//! \brief Holds declaration for all the AST Nodes
namespace sammine_lang {
namespace AST {
using Identifier = std::string;
class Printable;
class ProgramAST;
class DefinitionAST;
class VarDefAST;
class ExternAST;
class FuncDefAST;
class RecordDefAST;
class PrototypeAST;
class TypedVarAST;
class Stmt;
class ExprAST;
class CallExprAST;
class UnitExprAST;
class ReturnExprAST;
class BinaryExprAST;
class NumberExprAST;
class StringExprAST;
class BoolExprAST;
class VariableExprAST;
class BlockAST;
class IfExprAST;
class DerefExprAST;
class AddrOfExprAST;

} // namespace AST
} // namespace sammine_lang
