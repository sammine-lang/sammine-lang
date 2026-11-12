#include "codegen/MLIRGen.h"
#include "cabi/CABI.h"
#include "codegen/MLIRGenImpl.h"
#include "compiler/Compiler.h"

using sammine_util::cautious_at;
using sammine_util::cautious_value;

#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include "fmt/core.h"

#include <algorithm>
#include <string>

namespace sammine_lang {

// ===--- generate() ---===
// Entry point for MLIR code generation. Function bodies go through MIR
// (lowerThroughMIR); what is emitted here is the module around them: types,
// declarations, C bindings, kernels.
//
// Three pre-passes before emitting definitions:
// 1) Declare runtime functions (malloc, free, exit)
// 2) Register all struct/enum LLVM types (so forward refs work): declare each
//    struct, then fill in its fields
// 3) Forward-declare all function signatures (needed for function-as-value
// refs)

mlir::ModuleOp MLIRGenImpl::generate(AST::ProgramAST *program) {
  theModule = mlir::ModuleOp::create(builder.getUnknownLoc());

  declareRuntimeFunctions();

  // Register the closure struct type: { ptr, ptr }
  auto closurePtrTy = llvmPtrTy();
  closureType = mlir::LLVM::LLVMStructType::getIdentified(builder.getContext(),
                                                          kClosureTypeName);
  (void)closureType.setBody({closurePtrTy, closurePtrTy}, /*isPacked=*/false);

  // Pre-pass 1: register struct and enum types so that function
  // forward-declarations can reference them. A struct's fields can be any
  // struct or enum, in any order: generic instantiations come first in the
  // DefinitionVec, yet Box<Pt> needs Pt. So, as with functions, every struct
  // type is declared here, and its fields are filled in below. An enum's layout
  // only needs payload sizes, so it is complete here.
  for (auto &def : program->DefinitionVec) {
    if (auto *sd = llvm::dyn_cast<AST::StructDefAST>(def.get())) {
      if (sd->get_type().type_kind == TypeKind::Struct) {
        auto type = sd->get_type();
        auto &st = std::get<StructType>(type.type_data);
        structTypes[type] = mlir::LLVM::LLVMStructType::getIdentified(
            builder.getContext(),
            kStructTypePrefix.str() + st.get_name().mangled());
      }
    } else if (auto *ed = llvm::dyn_cast<AST::EnumDefAST>(def.get())) {
      if (ed->get_type().type_kind == TypeKind::Enum) {
        auto et = std::get<EnumType>(ed->get_type().type_data);
        // Integer-backed enums are bare integers — no struct registration
        if (et.is_integer_backed())
          continue;
        auto name = et.get_name().mangled();
        // Compute max payload size across all variants
        int64_t max_payload_size = 0;
        for (auto &vi : et.get_variants())
          max_payload_size = std::max(max_payload_size,
                                      layout::variantPayloadSize(vi.payload_types));
        // Enum layout: { i32 tag, [N x i8] payload }
        llvm::SmallVector<mlir::Type> fields;
        fields.push_back(builder.getI32Type());
        if (max_payload_size > 0)
          fields.push_back(mlir::LLVM::LLVMArrayType::get(
              builder.getI8Type(), static_cast<uint64_t>(max_payload_size)));
        auto enumTy = mlir::LLVM::LLVMStructType::getIdentified(
            builder.getContext(), "sammine.enum." + name);
        (void)enumTy.setBody(fields, /*isPacked=*/false);
        enumTypes[ed->get_type()] = enumTy;
      }
    }
  }
  for (auto &def : program->DefinitionVec) {
    auto *sd = llvm::dyn_cast<AST::StructDefAST>(def.get());
    if (!sd || sd->get_type().type_kind != TypeKind::Struct)
      continue;
    auto type = sd->get_type();
    llvm::SmallVector<mlir::Type> fieldTypes;
    for (auto &ft : std::get<StructType>(type.type_data).get_field_types())
      fieldTypes.push_back(convertType(ft));
    (void)structTypes[type].setBody(fieldTypes, /*isPacked=*/false);
  }

  // Pre-pass 2: forward-declare functions (now that all types are registered).
  for (auto &def : program->DefinitionVec) {
    if (auto *fd = llvm::dyn_cast<AST::FuncDefAST>(def.get())) {
      if (!fd->Prototype->is_generic())
        forwardDeclareFunc(fd->Prototype.get());
    } else if (auto *tci =
                   llvm::dyn_cast<AST::TypeClassInstanceAST>(def.get())) {
      if (!tci->is_generic())
        for (auto &method : tci->methods)
          forwardDeclareFunc(method->Prototype.get());
    } else if (auto *ext = llvm::dyn_cast<AST::ExternAST>(def.get())) {
      emitExtern(ext);
    } else if (auto *kd = llvm::dyn_cast<AST::KernelDefAST>(def.get())) {
      if (!kd->Prototype->is_generic()) {
        auto publicName = mangleName(kd->Prototype->functionName);
        auto internalName = kKernelPrefix.str() + publicName;

        // Create kernel module lazily on first kernel def
        if (!kernelModule)
          kernelModule = mlir::ModuleOp::create(builder.getUnknownLoc());

        // 1. Forward-declare internal kernel in kernel module (tensor types)
        auto kernelFuncType = buildKernelFuncType(kd->Prototype.get());
        if (!kernelModule.lookupSymbol(internalName)) {
          auto funcOp = mlir::func::FuncOp::create(
              builder.getUnknownLoc(), internalName, kernelFuncType);
          funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
          kernelModule.push_back(funcOp);
        }

        // 2. Forward-declare internal kernel in CPU module (memref types).
        // The wrapper calls __kernel_ with memref types; this declaration
        // ensures func.call resolves during verification. Replaced during
        // merge with the actual bufferized definition.
        auto memrefFuncType =
            buildKernelFuncType(kd->Prototype.get(), /*asMemref=*/true);
        if (!theModule.lookupSymbol(internalName)) {
          auto funcOp = mlir::func::FuncOp::create(
              builder.getUnknownLoc(), internalName, memrefFuncType);
          funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
          theModule.push_back(funcOp);
        }

        // 3. Forward-declare public wrapper in CPU module.
        // GPU: memref args so wrapper body is pure memref/gpu dialect.
        // CPU: !llvm.ptr args (kernel ABI — arrays by reference).
        {
          auto vis = moduleName.empty()
                         ? mlir::SymbolTable::Visibility::Private
                         : mlir::SymbolTable::Visibility::Public;

          mlir::FunctionType wrapperFuncType;
          if (targetGPU()) {
            wrapperFuncType = buildGpuWrapperFuncType(kd->Prototype.get());
          } else {
            // CPU: manually build the type with !llvm.ptr for arrays + sret
            llvm::SmallVector<mlir::Type, 4> argTypes;
            for (auto &param : kd->Prototype->parameterVectors) {
              if (param->get_type().type_kind == TypeKind::Array)
                argTypes.push_back(llvmPtrTy());
              else
                argTypes.push_back(convertType(param->get_type()));
            }
            llvm::SmallVector<mlir::Type, 1> retTypes;
            auto protoFT = std::get<FunctionType>(
                kd->Prototype->get_type().type_data);
            auto retSamType = protoFT.get_return_type();
            if (retSamType.type_kind == TypeKind::Array)
              argTypes.push_back(llvmPtrTy()); // sret
            else if (retSamType.type_kind != TypeKind::Unit)
              retTypes.push_back(convertType(retSamType));
            wrapperFuncType = builder.getFunctionType(argTypes, retTypes);
          }

          if (!theModule.lookupSymbol(publicName)) {
            auto funcOp = mlir::func::FuncOp::create(
                builder.getUnknownLoc(), publicName, wrapperFuncType);
            funcOp.setVisibility(vis);
            funcOp->setAttr("sammine.kernel_wrapper", builder.getUnitAttr());
            // An array it returns, it fills in a buffer passed last.
            if (std::get<FunctionType>(kd->Prototype->get_type().type_data)
                    .get_return_type()
                    .type_kind == TypeKind::Array)
              funcOp.setArgAttr(wrapperFuncType.getNumInputs() - 1,
                                smn::kResultBufferAttr, builder.getUnitAttr());
            theModule.push_back(funcOp);
          }
        }
      }
    }
  }

  // Every function body goes through MIR, before any definition is emitted
  // here, so a kernel's wrapper sees its callees' final signatures.
  lowerThroughMIR(program);

  for (auto &def : program->DefinitionVec)
    emitDefinition(def.get());

  if (kernelModule) {
    if (mlir::failed(mlir::verify(kernelModule))) {
      kernelModule.emitError("MLIR kernel module verification failed");
      return nullptr;
    }
  }

  if (mlir::failed(mlir::verify(theModule))) {
    theModule.emitError("MLIR module verification failed");
    return nullptr;
  }

  return theModule;
}

// ===--- Location helpers ---===

mlir::Location MLIRGenImpl::loc(AST::AstBase *ast) {
  return locator.loc(builder, ast);
}

// ===--- Runtime function declarations ---===

void MLIRGenImpl::declareRuntimeFunctions() {
  auto unknownLoc = builder.getUnknownLoc();
  auto ptrTy = llvmPtrTy();
  auto i32Ty = builder.getI32Type();
  auto i64Ty = builder.getI64Type();
  auto voidTy = llvmVoidTy();

  builder.setInsertionPointToEnd(theModule.getBody());

  // @malloc(i64) -> !llvm.ptr  [LLVM dialect — used by emitAllocExpr]
  {
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(ptrTy, {i64Ty});
    mlir::LLVM::LLVMFuncOp::create(builder, unknownLoc, kMallocFunc, fnTy);
  }
  // @free(!llvm.ptr) -> void  [LLVM dialect — used by emitFreeExpr]
  {
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(voidTy, {ptrTy});
    mlir::LLVM::LLVMFuncOp::create(builder, unknownLoc, kFreeFunc, fnTy);
  }
  // @dprintf(i32, !llvm.ptr, ...) -> i32  [LLVM dialect — used by the bounds
  // check to report to stderr's file descriptor]
  {
    auto fnTy = mlir::LLVM::LLVMFunctionType::get(i32Ty, {i32Ty, ptrTy},
                                                  /*isVarArg=*/true);
    mlir::LLVM::LLVMFuncOp::create(builder, unknownLoc, kDprintfFunc, fnTy);
  }
  // @exit(i32)  [func dialect — used by bounds check via func.call]
  {
    auto funcType = builder.getFunctionType({i32Ty}, {});
    auto funcOp = mlir::func::FuncOp::create(unknownLoc, kExitFunc, funcType);
    funcOp.setVisibility(mlir::SymbolTable::Visibility::Private);
    theModule.push_back(funcOp);
  }
}

// ===--- Type conversion ---===
// Maps sammine Types to MLIR types. Scalars → builtin integer/float types.
// Pointers → !llvm.ptr (opaque). Arrays → LLVMArrayType. Structs/enums → named
// LLVMStructType. Functions → closure fat pointer struct { code_ptr, env_ptr }.

mlir::Type MLIRGenImpl::convertType(const Type &type) {
  switch (type.type_kind) {
  case TypeKind::I32_t:
  case TypeKind::U32_t:
  case TypeKind::Integer:
    return builder.getI32Type();
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return builder.getI64Type();
  case TypeKind::F64_t:
  case TypeKind::Flt:
    return builder.getF64Type();
  case TypeKind::F32_t:
    return builder.getF32Type();
  case TypeKind::Bool:
    return builder.getI1Type();
  case TypeKind::Char:
    return builder.getI8Type();
  case TypeKind::Unit:
    return mlir::NoneType::get(builder.getContext());
  case TypeKind::String:
    // String is a pointer to i8 in LLVM
    return llvmPtrTy();
  case TypeKind::Pointer:
    return llvmPtrTy();
  case TypeKind::Function:
    // Function-typed values are represented as closure fat pointers
    return closureType;
  case TypeKind::Array: {
    auto &arrType = std::get<ArrayType>(type.type_data);
    auto elemMlirType = convertType(arrType.get_element());
    return mlir::LLVM::LLVMArrayType::get(elemMlirType, arrType.get_size());
  }
  case TypeKind::Struct:
    return cautious_at(structTypes, type, "structTypes");
  case TypeKind::Enum: {
    auto &et = std::get<EnumType>(type.type_data);
    if (et.is_integer_backed())
      return getEnumBackingMLIRType(et);
    return cautious_at(enumTypes, type, "enumTypes");
  }
  case TypeKind::Tuple: {
    auto &tt = std::get<TupleType>(type.type_data);
    std::vector<mlir::Type> elemTypes;
    for (auto &et : tt.get_element_types())
      elemTypes.push_back(convertType(et));
    return mlir::LLVM::LLVMStructType::getLiteral(builder.getContext(),
                                                  elemTypes);
  }
  case TypeKind::NonExistent:
    imm_error("type was never resolved (NonExistent) — "
              "this is a compiler bug: a type expression was not "
              "visited during type checking");
  case TypeKind::Poisoned:
    imm_error("poisoned type reached codegen — "
              "a type error should have been reported and "
              "compilation halted before codegen");
  case TypeKind::Generic:
    imm_error("generic template type reached codegen — "
              "this is a compiler bug: the generic definition was "
              "not monomorphized before codegen");
  case TypeKind::TypeParam:
    imm_error(fmt::format(
        "unresolved type parameter '{}' reached codegen — "
        "this is a compiler bug: the type parameter was not substituted "
        "during monomorphization",
        type.to_string()));
  case TypeKind::Hole:
    imm_error(fmt::format(
        "open type argument in '{}' reached codegen — "
        "this is a compiler bug: the type checker should have given it a "
        "type from its context or reported that it couldn't",
        type.to_string()));
  case TypeKind::Never:
    imm_error("'never' type reached codegen — "
              "this is a compiler bug: a diverging expression's "
              "type was not handled before codegen");
  default:
    imm_error(fmt::format("unhandled type kind in convertType: {}",
                          type.to_string()));
  }
}

bool MLIRGenImpl::isUnsignedIntegerType(const Type &type) {
  return type.type_kind == TypeKind::U32_t || type.type_kind == TypeKind::U64_t;
}

bool MLIRGenImpl::isFloatType(const Type &type) {
  return type.type_kind == TypeKind::F64_t ||
         type.type_kind == TypeKind::F32_t || type.type_kind == TypeKind::Flt;
}

mlir::Type MLIRGenImpl::getEnumBackingMLIRType(const EnumType &et) {
  switch (et.get_backing_type()) {
  case TypeKind::I32_t:
  case TypeKind::U32_t:
    return builder.getI32Type();
  case TypeKind::I64_t:
  case TypeKind::U64_t:
    return builder.getI64Type();
  default:
    imm_error("invalid enum backing type — expected i32, i64, u32, or u64");
  }
}

// ===--- Definition emission ---===

// The function bodies a definition is compiled to: a function's own, or an
// instance's methods. A generic function or instance has none: its copies
// for concrete types are definitions of their own. Lowering through MIR and
// emitting here both go by this, so they agree on what to compile.
static llvm::SmallVector<AST::FuncDefAST *, 1>
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

// Who can link to a definition's functions. A library's are global; an
// executable's and generic copies are internal, so two modules can both copy
// vec::push<i32>. An imported instance's methods are copies of its module's,
// there to inline.
MLIRGenImpl::Linkage MLIRGenImpl::functionLinkage(AST::DefinitionAST *def) {
  if (auto *tci = llvm::dyn_cast<AST::TypeClassInstanceAST>(def))
    return tci->is_declaration ? Linkage::AvailableExternally
           : moduleName.empty() || tci->is_instantiation
               ? Linkage::Internal
               : Linkage::Global;
  auto *fd = llvm::cast<AST::FuncDefAST>(def);
  return !moduleName.empty() && !fd->is_instantiation ? Linkage::Global
                                                       : Linkage::Internal;
}

void MLIRGenImpl::emitDefinition(AST::DefinitionAST *def) {
  if (llvm::isa<AST::FuncDefAST, AST::TypeClassInstanceAST>(def)) {
    auto linkage = functionLinkage(def);
    for (auto *fd : functionBodies(def))
      emitFunction(fd, linkage);
  } else if (auto *externDef = llvm::dyn_cast<AST::ExternAST>(def))
    emitExtern(externDef);
  else if (llvm::isa<AST::StructDefAST>(def))
    ; // Struct types registered in generate() pre-pass
  else if (llvm::isa<AST::EnumDefAST>(def))
    ; // Enum types registered in generate() pre-pass
  else if (llvm::isa<AST::TypeAliasDefAST>(def))
    ; // Type aliases resolved at type-check time
  else if (llvm::isa<AST::TypeClassDeclAST>(def))
    ; // Skip typeclass declarations (no codegen needed)
  else if (auto *kd = llvm::dyn_cast<AST::KernelDefAST>(def)) {
    emitKernelDef(kd);
  } else {
    imm_error(fmt::format("unknown definition type '{}'", def->getTreeName()),
              def->get_location());
  }
}

void MLIRGenImpl::lowerThroughMIR(AST::ProgramAST *program) {
  for (auto &def : program->DefinitionVec)
    for (auto *fd : functionBodies(def.get())) {
      auto name = mangleName(fd->Prototype->functionName);
      auto decl = theModule.lookupSymbol<mlir::func::FuncOp>(name);
      if (!decl || !decl.isDeclaration())
        continue;
      if (!mirLowering->lowerFunction(fd, decl))
        imm_error(fmt::format("the compiler can't compile '{}': MIR doesn't "
                              "lower what it does yet",
                              name),
                  fd->Prototype->get_location());
    }
}

// A function's declaration, in LLVM types: what MIR defines it as, once
// lowered, so a call from another module, which declares it from here,
// agrees. An array is returned by value, as MIR returns it.
mlir::FunctionType MLIRGenImpl::buildFuncType(AST::PrototypeAST *proto) {
  llvm::SmallVector<mlir::Type, 4> argTypes;
  for (auto &param : proto->parameterVectors)
    argTypes.push_back(convertType(param->get_type()));

  llvm::SmallVector<mlir::Type, 1> retTypes;
  if (proto->get_type().type_kind == TypeKind::Function) {
    auto retType =
        std::get<FunctionType>(proto->get_type().type_data).get_return_type();
    if (retType.type_kind != TypeKind::Unit)
      retTypes.push_back(convertType(retType));
  }
  return builder.getFunctionType(argTypes, retTypes);
}

std::string
MLIRGenImpl::mangleName(const sammine_util::QualifiedName &qn) const {
  return props_.symbol(qn);
}

// ===--- Expression dispatcher ---===
// Central dispatch for all expressions. Uses dynamic_cast chain (not visitor)
// because each emit* method returns an mlir::Value.

// ===--- Block and variable definition ---===

// ===--- Helpers ---===

// ===--- Public entry point ---===

MLIRGenResult mlirGen(mlir::MLIRContext &context, AST::ProgramAST *program,
                      const std::string &moduleName,
                      const std::string &fileName,
                      const std::string &sourceText,
                      const AST::ASTProperties &props,
                      GPUMode gpu_mode) {
  MLIRGenImpl impl(context, moduleName, fileName, sourceText, props, gpu_mode);
  auto cpuModule = impl.generate(program);
  if (!cpuModule)
    return {};
  return {cpuModule, impl.kernelModule};
}

} // namespace sammine_lang
