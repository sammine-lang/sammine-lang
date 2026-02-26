#pragma once
#include "codegen/LLVMRes.h"
#include "lex/Token.h"
#include "typecheck/Types.h"
#include "util/Utilities.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <map>

//! \file TypeConverter.h
//! \brief Defines the TypeConverter, which holds the characistics of converting
//! our AST types into LLVM IR types
namespace sammine_lang::AST {
class TypeConverter {

  llvm::LLVMContext &context;
  std::map<std::string, llvm::StructType *> named_struct_types;
  std::map<std::string, llvm::StructType *> named_enum_types;

public:
  llvm::Type *get_type(Type t);
  llvm::Type *get_return_type(Type t);
  llvm::FunctionType *get_closure_function_type(const FunctionType &ft);
  llvm::CmpInst::Predicate get_cmp_func(Type a, Type b, TokenType tok);

  void register_struct_type(const std::string &name,
                            llvm::StructType *llvm_type) {
    named_struct_types[name] = llvm_type;
  }
  llvm::StructType *get_struct_type(const std::string &name) const {
    auto it = named_struct_types.find(name);
    if (it != named_struct_types.end())
      return it->second;
    return nullptr;
  }

  void register_enum_type(const std::string &name,
                          llvm::StructType *llvm_type) {
    named_enum_types[name] = llvm_type;
  }
  llvm::StructType *get_enum_type(const std::string &name) const {
    auto it = named_enum_types.find(name);
    if (it != named_enum_types.end())
      return it->second;
    return nullptr;
  }

  TypeConverter(LLVMRes &resPtr) : context(*resPtr.Context.get()) {}
};
} // namespace sammine_lang::AST
