#include "codegen/ModuleGen.h"
#include "cabi/CABI.h"
#include "codegen/KernelGen.h"
#include "codegen/MIRLowering.h"
#include "codegen/MIRTypes.h"
#include "codegen/Runtime.h"
#include "codegen/SourceLocator.h"
#include "dialect/MIRToLLVM.h"
#include "util/Utilities.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"

#include "llvm/ADT/SmallVector.h"

#include "fmt/core.h"

#include <memory>
#include <optional>

//! \file ModuleGen.cpp
//! \brief The module around the code: the runtime's and every function's
//! declarations, C bindings, and who can link to each function. Bodies go
//! through MIR, kernels through KernelGen.

namespace sammine_lang {

namespace {

// The function bodies a definition is compiled to: a function's own, or an
// instance's methods. A generic function or instance has none: its copies
// for concrete types are definitions of their own.
llvm::SmallVector<AST::FuncDefAST *, 1>
functionBodies(AST::DefinitionAST *def) {
  llvm::SmallVector<AST::FuncDefAST *, 1> bodies;
  if (auto *fd = llvm::dyn_cast<AST::FuncDefAST>(def)) {
    if (!fd->Prototype->is_generic())
      bodies.push_back(fd);
  } else if (auto *tci = llvm::dyn_cast<AST::TypeClassInstanceAST>(def);
             tci && !tci->is_generic()) {
    for (auto &method : tci->methods)
      if (!method->Prototype->is_generic())
        bodies.push_back(method.get());
  }
  return bodies;
}

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

Type returnType(AST::PrototypeAST *proto) {
  return std::get<FunctionType>(proto->get_type().type_data).get_return_type();
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
  if (auto ext = cExtension(returnType(proto)))
    funcOp.setResultAttr(0, *ext, mlir::UnitAttr::get(ctx));
}

class ModuleGen {
public:
  ModuleGen(mlir::MLIRContext &context, const std::string &moduleName,
            const SourceLocator &locator, const std::string &fileName,
            const std::string &sourceText, const AST::ASTProperties &props,
            GPUMode gpuMode)
      : builder_(&context), library_(!moduleName.empty()), locator_(locator),
        reporter_(fileName, sourceText, 3), props_(props), types_(&context),
        mir_(builder_, props, locator_), gpuMode_(gpuMode) {}

  /// The CPU module, or null if the program asks for what can't be
  /// generated (reported()) or a module doesn't verify.
  mlir::ModuleOp generate(AST::ProgramAST *program);
  bool reported() const { return reported_; }
  mlir::ModuleOp kernelModule() const {
    return kernels_ ? kernels_->kernelModule() : nullptr;
  }

private:
  mlir::OpBuilder builder_;
  mlir::ModuleOp module_;
  /// Whether this is a library's module, whose functions other modules call.
  bool library_;
  const SourceLocator &locator_;
  sammine_util::Reporter reporter_;
  const AST::ASTProperties &props_;
  /// Each sammine type's MIR type, what the module declares functions in.
  MIRTypes types_;
  /// Lowers each function's body, and kernels' scalar code.
  MIRLowering mir_;
  GPUMode gpuMode_;
  std::unique_ptr<KernelGen> kernels_;
  bool reported_ = false;

  /// Who a function is for.
  enum class Linkage {
    /// Other modules link to it. In a library that is every function and
    /// instance method, private ones too, since the library's generic code
    /// compiled in another module may call them; privacy only limits who can
    /// name them. In an executable only main is.
    Global,
    /// Only this module: an executable's functions, and every module's own
    /// copy of a generic instantiation.
    Internal,
    /// A copy of another module's function, an imported instance's method,
    /// for the optimizer to inline; never emitted, as the module defines it.
    AvailableExternally,
  };

  [[noreturn]] void
  error(const std::string &msg,
        sammine_util::Location loc = sammine_util::Location(-1, -1)) {
    reporter_.immediate_error(msg, loc);
    sammine_util::abort(msg);
  }
  /// The symbol `qn` is defined and called as, as NameResolver decided it.
  std::string symbol(const sammine_util::QualifiedName &qn) const {
    return props_.symbol(qn);
  }
  mlir::LLVM::LLVMPointerType llvmPtr() {
    return mlir::LLVM::LLVMPointerType::get(builder_.getContext());
  }
  mlir::LLVM::LLVMVoidType llvmVoid() {
    return mlir::LLVM::LLVMVoidType::get(builder_.getContext());
  }
  /// `proto`'s type in MIR types, which sammine-mir-to-llvm converts.
  mlir::FunctionType mirFunctionType(AST::PrototypeAST *proto);
  /// The LLVM type of `type`, through its MIR type: for a C function's
  /// declaration, which has to be in LLVM's types already.
  mlir::Type llvmType(const Type &type);

  void declareRuntime();
  void declareFunction(AST::PrototypeAST *proto);
  void declareCBinding(AST::ExternAST *ast);
  /// Gives each function its body through MIR; one MIR can't lower is an
  /// error.
  void lowerBodies(AST::ProgramAST *program);
  Linkage linkage(AST::DefinitionAST *def) const;
  void setLinkage(AST::FuncDefAST *ast, Linkage linkage);
};

// Before any body is lowered, the runtime's functions and every function are
// declared, a function in MIR types, so a body can call or take any of them,
// and kernels' wrappers too. Then MIR gives each function its body, and each
// function its linkage.
mlir::ModuleOp ModuleGen::generate(AST::ProgramAST *program) {
  module_ = mlir::ModuleOp::create(builder_.getUnknownLoc());
  kernels_ = std::make_unique<KernelGen>(
      builder_, module_, mir_, props_, locator_, reporter_, gpuMode_, library_);
  declareRuntime();

  for (auto &def : program->DefinitionVec) {
    if (auto *ext = llvm::dyn_cast<AST::ExternAST>(def.get()))
      declareCBinding(ext);
    else if (auto *kd = llvm::dyn_cast<AST::KernelDefAST>(def.get()))
      kernels_->declare(kd);
    else
      for (auto *fd : functionBodies(def.get()))
        declareFunction(fd->Prototype.get());
  }

  // Every function body goes through MIR before any kernel's wrapper is
  // defined, so a wrapper sees its callees' final signatures.
  lowerBodies(program);

  for (auto &def : program->DefinitionVec) {
    auto *d = def.get();
    if (llvm::isa<AST::FuncDefAST, AST::TypeClassInstanceAST>(d)) {
      auto functionLinkage = linkage(d);
      for (auto *fd : functionBodies(d))
        setLinkage(fd, functionLinkage);
    } else if (auto *kd = llvm::dyn_cast<AST::KernelDefAST>(d)) {
      if (mlir::failed(kernels_->define(kd))) {
        reported_ = true;
        return nullptr;
      }
    } else if (!llvm::isa<AST::ExternAST, AST::StructDefAST, AST::EnumDefAST,
                          AST::TypeAliasDefAST, AST::TypeClassDeclAST>(d)) {
      error(fmt::format("unknown definition type '{}'", d->getTreeName()),
            d->get_location());
    }
  }

  if (auto kernels = kernelModule();
      kernels && mlir::failed(mlir::verify(kernels))) {
    kernels.emitError("MLIR kernel module verification failed");
    return nullptr;
  }
  if (mlir::failed(mlir::verify(module_))) {
    module_.emitError("MLIR module verification failed");
    return nullptr;
  }
  return module_;
}

// --- Types ---

mlir::FunctionType ModuleGen::mirFunctionType(AST::PrototypeAST *proto) {
  auto protoType = proto->get_type();
  auto *fn = std::get_if<FunctionType>(&protoType.type_data);
  auto type = fn ? types_.mirFunctionType(*fn) : std::nullopt;
  if (!type)
    error(fmt::format("the compiler has no type for '{}'",
                      proto->functionName.mangled()),
          proto->get_location());
  return *type;
}

mlir::Type ModuleGen::llvmType(const Type &type) {
  auto mir = types_.mirType(type);
  auto llvm = mir ? llvmTypeOf(*mir) : mlir::Type();
  if (!llvm)
    error(fmt::format("the compiler has no type for '{}'", type.to_string()));
  return llvm;
}

// --- Declarations ---

// malloc and free for allocation; dprintf and exit to report a failed bounds
// check, to stderr's file descriptor.
void ModuleGen::declareRuntime() {
  auto unknown = builder_.getUnknownLoc();
  auto i32 = builder_.getI32Type();
  builder_.setInsertionPointToEnd(module_.getBody());
  mlir::LLVM::LLVMFuncOp::create(
      builder_, unknown, runtime::kMalloc,
      mlir::LLVM::LLVMFunctionType::get(llvmPtr(), {builder_.getI64Type()}));
  mlir::LLVM::LLVMFuncOp::create(
      builder_, unknown, runtime::kFree,
      mlir::LLVM::LLVMFunctionType::get(llvmVoid(), {llvmPtr()}));
  mlir::LLVM::LLVMFuncOp::create(
      builder_, unknown, runtime::kDprintf,
      mlir::LLVM::LLVMFunctionType::get(i32, {i32, llvmPtr()},
                                        /*isVarArg=*/true));
  auto exit = mlir::func::FuncOp::create(unknown, runtime::kExit,
                                         builder_.getFunctionType({i32}, {}));
  exit.setVisibility(mlir::SymbolTable::Visibility::Private);
  module_.push_back(exit);
}

void ModuleGen::declareFunction(AST::PrototypeAST *proto) {
  auto name = symbol(proto->functionName);
  if (module_.lookupSymbol(name))
    return;
  // Private until it gets a body and its linkage. One that never gets a
  // body, an imported instance method, must stay private: MLIR doesn't
  // allow a public declaration.
  auto funcOp = mlir::func::FuncOp::create(builder_.getUnknownLoc(), name,
                                           mirFunctionType(proto));
  funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
  module_.push_back(funcOp);
}

void ModuleGen::declareCBinding(AST::ExternAST *ast) {
  // A C binding's symbol is its C name: std::printf is printf. The runtime
  // or another module's binding may have declared it already.
  auto name = symbol(ast->Prototype->functionName);
  if (module_.lookupSymbol(name))
    return;

  auto location = locator_.loc(builder_, ast);
  builder_.setInsertionPointToEnd(module_.getBody());
  auto *proto = ast->Prototype.get();
  auto result = returnType(proto);
  auto &params = proto->parameterVectors;
  auto isStruct = [](const Type &type) {
    return type.type_kind == TypeKind::Struct ||
           type.type_kind == TypeKind::Tuple;
  };
  auto isFunction = [](const Type &type) {
    return type.type_kind == TypeKind::Function;
  };
  // C takes a function as a plain pointer to its code, where sammine has a
  // closure; lowering passes the code of a C-callable copy (MIRToLLVM).
  auto cParamType = [&](const Type &type) {
    return isFunction(type) ? llvmPtr() : llvmType(type);
  };

  // C moves a struct in registers, as the type cabi::registerType gives, or
  // through memory: a returned one through a pointer the caller passes as a
  // hidden first argument (sret), an argument as a pointer to a copy
  // (byval). An argument struct also goes through memory when the registers
  // it needs are no longer all free. LLVM leaves this to the frontend.
  if (ast->is_c_function &&
      (isStruct(result) || llvm::any_of(params, [&](auto &param) {
         return isStruct(param->get_type()) || isFunction(param->get_type());
       }))) {
    mlir::DataLayout layout(module_);
    llvm::SmallVector<mlir::Type, 4> paramTypes;
    mlir::Type cResult = llvmVoid();
    mlir::Type sretType;
    if (isStruct(result)) {
      auto structType = llvmType(result);
      if (auto regType = cabi::registerType(structType, layout)) {
        cResult = regType;
      } else {
        sretType = structType;
        paramTypes.push_back(llvmPtr());
      }
    } else if (result.type_kind != TypeKind::Unit) {
      cResult = llvmType(result);
    }

    // Registers still free for arguments; the sret pointer takes one.
    cabi::Registers free{cabi::kIntegerArgRegisters, cabi::kFloatArgRegisters};
    if (sretType)
      free.integer--;
    auto take = [&](const cabi::Registers &needed) {
      free.integer -= std::min(free.integer, needed.integer);
      free.floating -= std::min(free.floating, needed.floating);
    };

    llvm::SmallVector<std::pair<unsigned, mlir::Type>> byvalTypes;
    for (auto &param : params) {
      auto paramType = cParamType(param->get_type());
      if (!isStruct(param->get_type())) {
        take(cabi::registersFor(paramType));
        paramTypes.push_back(paramType);
        continue;
      }
      auto regType = cabi::registerType(paramType, layout);
      auto needed = regType ? cabi::registersFor(regType) : cabi::Registers{};
      if (regType && needed.integer <= free.integer &&
          needed.floating <= free.floating) {
        take(needed);
        paramTypes.push_back(regType);
      } else {
        byvalTypes.push_back({paramTypes.size(), paramType});
        paramTypes.push_back(llvmPtr());
      }
    }

    auto funcOp = mlir::LLVM::LLVMFuncOp::create(
        builder_, location, name,
        mlir::LLVM::LLVMFunctionType::get(cResult, paramTypes,
                                          proto->is_var_arg));
    if (sretType)
      funcOp.setArgAttr(0, mlir::LLVM::LLVMDialect::getStructRetAttrName(),
                        mlir::TypeAttr::get(sretType));
    for (auto &[index, byvalType] : byvalTypes)
      funcOp.setArgAttr(index, mlir::LLVM::LLVMDialect::getByValAttrName(),
                        mlir::TypeAttr::get(byvalType));
    markCExtensions(funcOp, proto, sretType ? 1 : 0);
    return;
  }

  // A variadic function is an llvm.func: func.func has no `...`.
  if (proto->is_var_arg) {
    llvm::SmallVector<mlir::Type, 4> paramTypes;
    for (auto &param : params)
      paramTypes.push_back(ast->is_c_function ? cParamType(param->get_type())
                                              : llvmType(param->get_type()));
    auto llvmResult =
        result.type_kind == TypeKind::Unit ? llvmVoid() : llvmType(result);
    auto funcOp = mlir::LLVM::LLVMFuncOp::create(
        builder_, location, name,
        mlir::LLVM::LLVMFunctionType::get(llvmResult, paramTypes,
                                          /*isVarArg=*/true));
    if (ast->is_c_function)
      markCExtensions(funcOp, proto, 0);
    return;
  }

  auto funcOp =
      mlir::func::FuncOp::create(location, name, mirFunctionType(proto));
  funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
  module_.push_back(funcOp);
  if (ast->is_c_function)
    markCExtensions(funcOp, proto, 0);
}

// --- Bodies and linkage ---

void ModuleGen::lowerBodies(AST::ProgramAST *program) {
  for (auto &def : program->DefinitionVec)
    for (auto *fd : functionBodies(def.get())) {
      auto name = symbol(fd->Prototype->functionName);
      auto decl = module_.lookupSymbol<mlir::func::FuncOp>(name);
      if (!decl || !decl.isDeclaration())
        continue;
      if (!mir_.lowerFunction(fd, decl))
        error(fmt::format("the compiler can't compile '{}': MIR doesn't "
                          "lower what it does yet",
                          name),
              fd->Prototype->get_location());
    }
}

// A library's functions are global; an executable's and generic copies are
// internal, so two modules can both copy vec::push<i32>. An imported
// instance's methods are copies of its module's, there to inline.
ModuleGen::Linkage ModuleGen::linkage(AST::DefinitionAST *def) const {
  if (auto *tci = llvm::dyn_cast<AST::TypeClassInstanceAST>(def))
    return tci->is_declaration                  ? Linkage::AvailableExternally
           : !library_ || tci->is_instantiation ? Linkage::Internal
                                                : Linkage::Global;
  auto *fd = llvm::cast<AST::FuncDefAST>(def);
  return library_ && !fd->is_instantiation ? Linkage::Global
                                           : Linkage::Internal;
}

void ModuleGen::setLinkage(AST::FuncDefAST *ast, Linkage linkage) {
  auto name = symbol(ast->Prototype->functionName);
  auto funcOp = module_.lookupSymbol<mlir::func::FuncOp>(name);
  if (name == "main")
    linkage = Linkage::Global;
  funcOp.setVisibility(linkage == Linkage::Internal
                           ? mlir::SymbolTable::Visibility::Private
                           : mlir::SymbolTable::Visibility::Public);
  // MLIR visibility doesn't reach LLVM, so set the linkage too.
  if (linkage != Linkage::Global)
    funcOp->setAttr("llvm.linkage",
                    mlir::LLVM::LinkageAttr::get(
                        builder_.getContext(),
                        linkage == Linkage::Internal
                            ? mlir::LLVM::Linkage::Internal
                            : mlir::LLVM::Linkage::AvailableExternally));
}

} // namespace

GeneratedModules
generateModules(mlir::MLIRContext &context, AST::ProgramAST *program,
                const std::string &moduleName, const SourceLocator &locator,
                const std::string &fileName, const std::string &sourceText,
                const AST::ASTProperties &props, GPUMode gpuMode) {
  ModuleGen gen(context, moduleName, locator, fileName, sourceText, props,
                gpuMode);
  auto cpuModule = gen.generate(program);
  if (!cpuModule)
    return {.cpuModule = {}, .kernelModule = {}, .reported = gen.reported()};
  return {cpuModule, gen.kernelModule()};
}

} // namespace sammine_lang
