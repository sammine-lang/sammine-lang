#include "codegen/TypeConverter.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>

//! \file TypeConverter.cpp
//! \brief Implementation for TypeConverter, converting AST Node types into LLVM
//! IR Types
namespace sammine_lang::AST {
llvm::Type *TypeConverter::get_type(Type t) {
  switch (t.type_kind) {
  case TypeKind::I32_t:
  case TypeKind::U32_t:
    return llvm::Type::getInt32Ty(context);
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return llvm::Type::getInt64Ty(context);
  case TypeKind::F64_t:
    return llvm::Type::getDoubleTy(context);
  case TypeKind::Unit:
    return llvm::Type::getVoidTy(context);
  case TypeKind::Bool:
    return llvm::Type::getInt1Ty(context);
  case TypeKind::Char:
    return llvm::Type::getInt8Ty(context);
  case TypeKind::String:
    return llvm::StructType::get(context, llvm::PointerType::getInt8Ty(context),
                                 llvm::Type::getInt32Ty(context));
  case TypeKind::Pointer:
    return llvm::PointerType::get(context, 0);
  case TypeKind::Array: {
    auto arr = std::get<ArrayType>(t.type_data);
    return llvm::ArrayType::get(get_type(arr.get_element()), arr.get_size());
  }
  case TypeKind::Function:
    return llvm::StructType::getTypeByName(context, "sammine.closure");
  case TypeKind::Struct: {
    auto &st = std::get<StructType>(t.type_data);
    auto *cached = get_struct_type(st.get_name().mangled());
    if (cached)
      return cached;
    sammine_util::abort(
        fmt::format("Struct '{}' not registered in TypeConverter",
                    st.get_name().mangled()));
  }
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(t.type_data);
    auto *cached = get_enum_type(et.get_name().mangled());
    if (cached)
      return cached;
    sammine_util::abort(
        fmt::format("Enum '{}' not registered in TypeConverter",
                    et.get_name().mangled()));
  }
  case TypeKind::Tuple:
    sammine_util::abort("Tuple type not supported in old LLVM codegen");
  case TypeKind::Never:
    sammine_util::abort("Never type should not reach codegen");
  case TypeKind::NonExistent:
    sammine_util::abort("Existed a type that is not synthesized yet");
  case TypeKind::Poisoned:
    sammine_util::abort("Poisoned typed should not be here");
  case TypeKind::Integer:
  case TypeKind::Flt:
    sammine_util::abort("Polymorphic literal type should not reach codegen");
  case TypeKind::TypeParam:
    sammine_util::abort("TypeParam should not reach codegen");
  }
  sammine_util::abort("Guarded by default case");
}
llvm::FunctionType *
TypeConverter::get_closure_function_type(const FunctionType &ft) {
  // Returns: ret_type (ptr, param_types...) — env pointer prepended
  std::vector<llvm::Type *> params;
  params.push_back(llvm::PointerType::get(context, 0)); // env ptr
  for (auto &p : ft.get_params_types())
    params.push_back(get_type(p));
  auto ret = ft.get_return_type();
  llvm::Type *ret_type = ret.type_kind == TypeKind::Unit
                             ? llvm::Type::getVoidTy(context)
                             : get_type(ret);
  return llvm::FunctionType::get(ret_type, params, false);
}

llvm::Type *TypeConverter::get_return_type(Type t) {
  switch (t.type_kind) {
  case TypeKind::Function:
    return get_type(std::get<FunctionType>(t.type_data).get_return_type());
  default:
    sammine_util::abort(
        "Jasmine passed in something that is not a function type");
  }
}
llvm::CmpInst::Predicate TypeConverter::get_cmp_func(Type a, Type b,
                                                     TokenType tok) {
  sammine_util::abort_if_not(a.type_kind == b.type_kind,
                             "Two types needs to be the same");
  using llvm::CmpInst;

  switch (a.type_kind) {

  case TypeKind::I32_t:
  case TypeKind::I64_t:
  case TypeKind::Bool:
  case TypeKind::Char: {
    // Signed integer comparisons
    switch (tok) {
    case TokenType::TokEQUAL:
      return CmpInst::ICMP_EQ;
    case TokenType::TokNOTEqual:
      return CmpInst::ICMP_NE;
    case TokenType::TokLESS:
      return CmpInst::ICMP_SLT;
    case TokenType::TokLessEqual:
      return CmpInst::ICMP_SLE;
    case TokenType::TokGREATER:
      return CmpInst::ICMP_SGT;
    case TokenType::TokGreaterEqual:
      return CmpInst::ICMP_SGE;
    default:
      sammine_util::abort("Invalid token for integer comparison");
    }
  }
  case TypeKind::U32_t:
  case TypeKind::U64_t: {
    // Unsigned integer comparisons
    switch (tok) {
    case TokenType::TokEQUAL:
      return CmpInst::ICMP_EQ;
    case TokenType::TokNOTEqual:
      return CmpInst::ICMP_NE;
    case TokenType::TokLESS:
      return CmpInst::ICMP_ULT;
    case TokenType::TokLessEqual:
      return CmpInst::ICMP_ULE;
    case TokenType::TokGREATER:
      return CmpInst::ICMP_UGT;
    case TokenType::TokGreaterEqual:
      return CmpInst::ICMP_UGE;
    default:
      sammine_util::abort("Invalid token for unsigned integer comparison");
    }
  }
  case TypeKind::F64_t: {
    switch (tok) {
    case TokenType::TokEQUAL:
      return CmpInst::FCMP_OEQ;
    case TokenType::TokNOTEqual:
      return CmpInst::FCMP_ONE;
    case TokenType::TokLESS:
      return CmpInst::FCMP_OLT;
    case TokenType::TokLessEqual:
      return CmpInst::FCMP_OLE;
    case TokenType::TokGREATER:
      return CmpInst::FCMP_OGT;
    case TokenType::TokGreaterEqual:
      return CmpInst::FCMP_OGE;
    default:
      sammine_util::abort("Invalid token for float comparison");
    }
  }
  case TypeKind::Pointer: {
    switch (tok) {
    case TokenType::TokEQUAL:
      return CmpInst::ICMP_EQ;
    case TokenType::TokNOTEqual:
      return CmpInst::ICMP_NE;
    default:
      sammine_util::abort("Only == and != supported for pointers");
    }
  }
  case TypeKind::Unit:
  case TypeKind::Function:
  case TypeKind::Array:
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::String:
  case TypeKind::Integer:
  case TypeKind::Flt:
  case TypeKind::Struct:
  case TypeKind::Enum:
  case TypeKind::Tuple:
  case TypeKind::TypeParam:
    sammine_util::abort(
        fmt::format("Cannot compare values of this type: {}", a.to_string()));
  }
  sammine_util::abort("End of get_cmp_func reached");
}
} // namespace sammine_lang::AST
