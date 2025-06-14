#pragma once
#include "codegen/LLVMRes.h"
#include "lex/Token.h"
#include "typecheck/Types.h"
#include "util/Utilities.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>

//! \file TypeConverter.h
//! \brief Defines the TypeConverter, which holds the characistics of converting
//! our AST types into LLVM IR types
namespace sammine_lang::AST {
class TypeConverter {

  llvm::LLVMContext &context;

public:
  llvm::Type *get_type(Type t);
  llvm::Type *get_return_type(Type t);
  llvm::CmpInst::Predicate get_cmp_func(Type a, Type b, TokenType tok);

  TypeConverter(LLVMRes &resPtr) : context(*resPtr.Context.get()) {}
};
} // namespace sammine_lang::AST
