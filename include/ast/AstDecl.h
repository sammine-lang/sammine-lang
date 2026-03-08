#pragma once
#include <cstdint>

//! \file AstDecl.h
//! \brief Holds declaration for all the AST Nodes
namespace sammine_lang {
namespace AST {

using NodeId = uint32_t;

class Printable;
class ProgramAST;
class DefinitionAST;
class VarDefAST;
class ExternAST;
class FuncDefAST;
class StructDefAST;
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
class CharExprAST;
class VariableExprAST;
class BlockAST;
class IfExprAST;
class DerefExprAST;
class AddrOfExprAST;
class AllocExprAST;
class FreeExprAST;
class ArrayLiteralExprAST;
class IndexExprAST;
class LenExprAST;
class DimExprAST;
class UnaryNegExprAST;
class StructLiteralExprAST;
class FieldAccessExprAST;
class CaseExprAST;
class WhileExprAST;
class TupleLiteralExprAST;
class EnumDefAST;
class TypeAliasDefAST;
class TypeClassDeclAST;
class TypeClassInstanceAST;
class KernelExprAST;
class KernelNumberExprAST;
class KernelBlockAST;
class KernelDefAST;

} // namespace AST
} // namespace sammine_lang
