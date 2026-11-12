#include "codegen/MLIRGenImpl.h"
#include "cabi/CABI.h"
#include "cabi/KernelABI.h"
#include "codegen/MLIRGenBinaryOps.h"
#include "dialect/SammineDialect.h"
#include "typecheck/BuiltinInstances.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinTypes.h"

#include "llvm/ADT/SmallVector.h"

#include "fmt/core.h"

namespace sammine_lang {

void MLIRGenImpl::forwardDeclareFunc(AST::PrototypeAST *proto) {
  if (proto->is_generic())
    return;
  auto funcType = buildFuncType(proto);
  auto funcName = mangleName(proto->functionName);
  if (!theModule.lookupSymbol(funcName)) {
    builder.setInsertionPointToEnd(theModule.getBody());
    // Private until emitFunction gives it a body and decides who can link to
    // it. One that never gets a body, an imported instance method, must stay
    // private: MLIR doesn't allow a public declaration.
    auto funcOp =
        mlir::func::FuncOp::create(builder.getUnknownLoc(), funcName, funcType);
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
    theModule.push_back(funcOp);
  }
}

// ===--- Function emission ---===

// Sets who can link to a function, whose body lowerThroughMIR gave it.
void MLIRGenImpl::emitFunction(AST::FuncDefAST *ast, Linkage linkage) {
  std::string funcName = mangleName(ast->Prototype->functionName);
  auto funcOp = theModule.lookupSymbol<mlir::func::FuncOp>(funcName);
  // MLIR visibility doesn't reach LLVM, so set the linkage too.
  if (funcName == "main")
    linkage = Linkage::Global;
  funcOp.setVisibility(linkage == Linkage::Internal
                           ? mlir::SymbolTable::Visibility::Private
                           : mlir::SymbolTable::Visibility::Public);
  if (linkage != Linkage::Global)
    funcOp->setAttr("llvm.linkage",
                    mlir::LLVM::LinkageAttr::get(
                        builder.getContext(),
                        linkage == Linkage::Internal
                            ? mlir::LLVM::Linkage::Internal
                            : mlir::LLVM::Linkage::AvailableExternally));
}

namespace {

// C expects a char or bool argument or result already extended to 32 bits
// by whoever produces it: signed for char, unsigned for bool. LLVM extends
// it when the declaration says so.
std::optional<llvm::StringRef> cExtension(const Type &type) {
  if (type.type_kind == TypeKind::Char)
    return mlir::LLVM::LLVMDialect::getSExtAttrName();
  if (type.type_kind == TypeKind::Bool)
    return mlir::LLVM::LLVMDialect::getZExtAttrName();
  return std::nullopt;
}

// Marks the char and bool parameters and result of a C function. Its
// declared parameters start at `firstParam`, after a hidden sret pointer.
template <typename FuncOp>
void markCExtensions(FuncOp funcOp, AST::PrototypeAST *proto,
                     unsigned firstParam) {
  auto *ctx = funcOp.getContext();
  auto &params = proto->parameterVectors;
  for (unsigned i = 0; i < params.size(); i++)
    if (auto ext = cExtension(params[i]->get_type()))
      funcOp.setArgAttr(firstParam + i, *ext, mlir::UnitAttr::get(ctx));
  auto retType =
      std::get<FunctionType>(proto->get_type().type_data).get_return_type();
  if (auto ext = cExtension(retType))
    funcOp.setResultAttr(0, *ext, mlir::UnitAttr::get(ctx));
}

} // namespace

void MLIRGenImpl::emitExtern(AST::ExternAST *ast) {
  // A C binding's symbol is its C name: std::printf is printf.
  std::string funcName = mangleName(ast->Prototype->functionName);

  // Skip if already declared (e.g. by declareRuntimeFunctions, or by another
  // module's binding of the same C function)
  if (theModule.lookupSymbol(funcName))
    return;

  auto location = loc(ast);
  builder.setInsertionPointToEnd(theModule.getBody());

  // C moves a struct in registers, as the type cabi::registerType gives, or
  // through memory: a returned one through a pointer the caller passes as a
  // hidden first argument (sret), an argument as a pointer to a copy
  // (byval). An argument struct also goes through memory when the registers
  // it needs are no longer all free. LLVM leaves this to the frontend.
  auto retType = std::get<FunctionType>(ast->Prototype->get_type().type_data)
                     .get_return_type();
  auto &params = ast->Prototype->parameterVectors;
  auto is_struct = [](const Type &type) {
    return type.type_kind == TypeKind::Struct ||
           type.type_kind == TypeKind::Tuple;
  };
  if (ast->is_c_function &&
      (is_struct(retType) || llvm::any_of(params, [&](auto &param) {
         return is_struct(param->get_type());
       }))) {
    mlir::DataLayout layout(theModule);
    llvm::SmallVector<mlir::Type, 4> paramTypes;
    mlir::Type cRetType = llvmVoidTy();
    mlir::Type sretType;
    if (is_struct(retType)) {
      auto structTy = convertType(retType);
      if (auto regTy = cabi::registerType(structTy, layout)) {
        cRetType = regTy;
      } else {
        sretType = structTy;
        paramTypes.push_back(llvmPtrTy());
      }
    } else if (retType.type_kind != TypeKind::Unit) {
      cRetType = convertType(retType);
    }

    // Registers still free for arguments; the sret pointer takes one.
    cabi::Registers free{cabi::kIntegerArgRegisters,
                         cabi::kFloatArgRegisters};
    if (sretType)
      free.integer--;
    auto take = [&](const cabi::Registers &needed) {
      free.integer -= std::min(free.integer, needed.integer);
      free.floating -= std::min(free.floating, needed.floating);
    };

    llvm::SmallVector<std::pair<unsigned, mlir::Type>> byvalTypes;
    for (auto &param : params) {
      auto paramTy = convertType(param->get_type());
      if (!is_struct(param->get_type())) {
        take(cabi::registersFor(paramTy));
        paramTypes.push_back(paramTy);
        continue;
      }
      auto regTy = cabi::registerType(paramTy, layout);
      auto needed = regTy ? cabi::registersFor(regTy) : cabi::Registers{};
      if (regTy && needed.integer <= free.integer &&
          needed.floating <= free.floating) {
        take(needed);
        paramTypes.push_back(regTy);
      } else {
        byvalTypes.push_back({paramTypes.size(), paramTy});
        paramTypes.push_back(llvmPtrTy());
      }
    }

    auto fnTy = mlir::LLVM::LLVMFunctionType::get(cRetType, paramTypes,
                                                  ast->Prototype->is_var_arg);
    auto funcOp =
        mlir::LLVM::LLVMFuncOp::create(builder, location, funcName, fnTy);
    if (sretType)
      funcOp.setArgAttr(0, mlir::LLVM::LLVMDialect::getStructRetAttrName(),
                        mlir::TypeAttr::get(sretType));
    for (auto &[index, byvalType] : byvalTypes)
      funcOp.setArgAttr(index, mlir::LLVM::LLVMDialect::getByValAttrName(),
                        mlir::TypeAttr::get(byvalType));
    markCExtensions(funcOp, ast->Prototype.get(), sretType ? 1 : 0);
    return;
  }

  if (ast->Prototype->is_var_arg) {
    // Vararg C functions must use LLVM dialect (func dialect has no varargs)
    llvm::SmallVector<mlir::Type, 4> paramTypes;
    for (auto &param : ast->Prototype->parameterVectors)
      paramTypes.push_back(convertType(param->get_type()));
    mlir::Type llvmRetType;
    if (retType.type_kind == TypeKind::Unit)
      llvmRetType = llvmVoidTy();
    else
      llvmRetType = convertType(retType);
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(llvmRetType, paramTypes,
                                                  /*isVarArg=*/true);
    auto funcOp =
        mlir::LLVM::LLVMFuncOp::create(builder, location, funcName, fnTy);
    if (ast->is_c_function)
      markCExtensions(funcOp, ast->Prototype.get(), 0);
  } else {
    auto funcType = buildFuncType(ast->Prototype.get());
    auto funcOp = mlir::func::FuncOp::create(
        location, llvm::StringRef(funcName), funcType);
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
    theModule.push_back(funcOp);
    if (ast->is_c_function)
      markCExtensions(funcOp, ast->Prototype.get(), 0);
  }
}


} // namespace sammine_lang
