//
// Created by Jasmine Tang on 3/27/24.
//

#include "codegen/CodegenVisitor.h"
#include "ast/Ast.h"
#include "codegen/CodegenUtils.h"
#include "util/Logging.h"
#include "util/Utilities.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include <llvm/IR/BasicBlock.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "codegen"

//! \file CodegenVisitor.cpp
//! \brief Implementation for CodegenVisitor, it converts the AST Representation
//! into LLVM IR and it also uses a visitor pattern in order to traverse through
//! the parsed AST to emit LLVM IR.
namespace sammine_lang::AST {
using llvm::BasicBlock;

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
///
/// from the llvm blog:
/// mem2reg only looks for alloca instructions in the entry block of the
/// function. Being in the entry block guarantees that the alloca is only
/// executed once, which makes analysis simpler.

llvm::Function *CgVisitor::getCurrentFunction() { return this->current_func; }

llvm::Function *CgVisitor::getOrCreateClosureWrapper(llvm::Function *fn,
                                                     const FunctionType &ft) {
  auto name = std::string(fn->getName());
  auto it = closure_wrappers.find(name);
  if (it != closure_wrappers.end()) {
    LOG({
      fmt::print(stderr, "[codegen] closure wrapper: cache hit for '{}'\n",
                 name);
    });
    return it->second;
  }

  llvm::IRBuilderBase::InsertPointGuard guard(*resPtr->Builder);

  // Create wrapper function: ret_type @__wrap_<name>(ptr %env, params...)
  auto *wrapperFT = type_converter.get_closure_function_type(ft);
  auto wrapperName = "__wrap_" + name;
  auto *wrapper =
      llvm::Function::Create(wrapperFT, llvm::Function::InternalLinkage,
                             wrapperName, resPtr->Module.get());

  auto *entry = llvm::BasicBlock::Create(*resPtr->Context, "entry", wrapper);
  resPtr->Builder->SetInsertPoint(entry);

  // Forward all args except env to the original function
  std::vector<llvm::Value *> args;
  for (auto it = wrapper->arg_begin() + 1; it != wrapper->arg_end(); ++it)
    args.push_back(&*it);

  if (fn->getReturnType()->isVoidTy()) {
    resPtr->Builder->CreateCall(fn, args);
    resPtr->Builder->CreateRetVoid();
  } else {
    auto *result = resPtr->Builder->CreateCall(fn, args, "wrap_call");
    resPtr->Builder->CreateRet(result);
  }

  LOG({
    fmt::print(stderr, "[codegen] closure wrapper: creating __wrap_{}\n", name);
  });
  closure_wrappers[name] = wrapper;
  return wrapper;
}
void CgVisitor::enter_new_scope() { allocaValues.push({}); }
void CgVisitor::exit_new_scope() { allocaValues.pop(); }

void CgVisitor::setCurrentFunction(llvm::Function *func) {
  this->current_func = func;
}
void CgVisitor::visit(FuncDefAST *ast) {
  // Skip generic templates — only monomorphized copies get codegen'd
  if (ast->Prototype->is_generic())
    return;

  current_func_exported = ast->is_exported;
  this->enter_new_scope();
  ast->Prototype->accept_vis(this);
  ast->walk_with_preorder(this);
  ast->Block->accept_vis(this);
  ast->walk_with_postorder(this);
  this->exit_new_scope();
  current_func_exported = false;
}

void CgVisitor::visit(TypeClassInstanceAST *ast) {
  // Codegen each instance method as a regular function
  for (auto &method : ast->methods)
    method->accept_vis(this);
}

void CgVisitor::preorder_walk(ProgramAST *ast) {
  // Create the named closure struct type: { ptr code, ptr env }
  auto *ptrTy = llvm::PointerType::get(*this->resPtr->Context, 0);
  llvm::StructType::create(*this->resPtr->Context, {ptrTy, ptrTy},
                           "sammine.closure");

  // Declare runtime functions
  CodegenUtils::declare_fn(*this->resPtr->Module, "printf",
                           llvm::Type::getInt32Ty(*this->resPtr->Context),
                           ptrTy, true);

  CodegenUtils::declare_malloc(*this->resPtr->Module);
  CodegenUtils::declare_free(*this->resPtr->Module);

  CodegenUtils::declare_fn(*this->resPtr->Module, "exit",
                           llvm::Type::getVoidTy(*this->resPtr->Context),
                           llvm::Type::getInt32Ty(*this->resPtr->Context),
                           false);

  // Forward-declare all functions so definition order doesn't matter.
  // Struct types must be registered first since prototypes reference them.
  for (auto &def : ast->DefinitionVec) {
    if (auto *sd = dynamic_cast<StructDefAST *>(def.get())) {
      if (sd->type.type_kind != TypeKind::Poisoned) {
        auto &st = std::get<StructType>(sd->type.type_data);
        if (!llvm::StructType::getTypeByName(*resPtr->Context,
                                             "sammine.struct." + st.get_name())) {
          std::vector<llvm::Type *> field_types;
          for (auto &ft : st.get_field_types())
            field_types.push_back(type_converter.get_type(ft));
          auto *llvm_struct = llvm::StructType::create(
              *resPtr->Context, field_types,
              "sammine.struct." + st.get_name());
          type_converter.register_struct_type(st.get_name(), llvm_struct);
        }
      }
    }
  }
  for (auto &def : ast->DefinitionVec) {
    if (auto *fd = dynamic_cast<FuncDefAST *>(def.get()))
      forward_declare(fd->Prototype.get());
    else if (auto *ext = dynamic_cast<ExternAST *>(def.get()))
      forward_declare(ext->Prototype.get());
    else if (auto *tci = dynamic_cast<TypeClassInstanceAST *>(def.get())) {
      for (auto &method : tci->methods)
        forward_declare(method->Prototype.get());
    }
  }
}

void CgVisitor::preorder_walk(VarDefAST *ast) {
  auto var_name = ast->TypedVar->name;
  LOG({
    fmt::print(stderr, "[CODEGEN] Codegen preorder_walk for VarDefAST for {}\n",
               var_name);
  });
  auto *alloca = CodegenUtils::CreateEntryBlockAlloca(
      getCurrentFunction(), var_name, type_converter.get_type(ast->type));
  this->allocaValues.top()[var_name] = alloca;
}
void CgVisitor::postorder_walk(VarDefAST *ast) {
  auto var_name = ast->TypedVar->name;
  auto *alloca = this->allocaValues.top()[var_name];

  LOG({
    fmt::print(stderr,
               "[CODEGEN] Codegen postorder_walk for VarDefAST for {}\n",
               var_name);
  });
  this->abort_if_not(ast->Expression,
                     "VarDefAST requires an initializer expression");

  resPtr->Builder->CreateStore(ast->Expression->val, alloca);
}
void CgVisitor::preorder_walk(ExternAST *ast) { in_reuse = true; }
void CgVisitor::preorder_walk(StructDefAST *ast) {
  if (ast->type.type_kind == TypeKind::Poisoned)
    return;

  auto &st = std::get<StructType>(ast->type.type_data);

  // Skip if already registered (e.g. by forward-declaration pass)
  if (type_converter.get_struct_type(st.get_name()))
    return;

  std::vector<llvm::Type *> field_llvm_types;
  for (auto &ft : st.get_field_types())
    field_llvm_types.push_back(type_converter.get_type(ft));

  auto *llvm_struct = llvm::StructType::create(
      *resPtr->Context, field_llvm_types,
      "sammine.struct." + st.get_name());
  type_converter.register_struct_type(st.get_name(), llvm_struct);
}
void CgVisitor::postorder_walk(ExternAST *ast) {
  in_reuse = false;
  // For reuse declarations in library modules, create an IFunc so the mangled
  // name (module$func) resolves to the original C symbol at load time.
  // We can't use GlobalAlias because the aliasee can't be a declaration.
  // IFuncs work portably: ELF resolves at load time, MachO uses
  // .symbol_resolver stubs.
  if (!module_name.empty()) {
    auto *fn = resPtr->Module->getFunction(ast->Prototype->functionName);
    if (fn) {
      std::string mangled = module_name + "$" + ast->Prototype->functionName;

      // Create resolver: define ptr @resolve_<mangled>() { ret ptr @fn }
      auto *ptrTy = llvm::PointerType::get(*resPtr->Context, 0);
      auto *resolverTy = llvm::FunctionType::get(ptrTy, false);
      auto *resolver = llvm::Function::Create(
          resolverTy, llvm::Function::InternalLinkage,
          "resolve_" + mangled, resPtr->Module.get());
      auto *entry =
          llvm::BasicBlock::Create(*resPtr->Context, "entry", resolver);

      {
        llvm::IRBuilderBase::InsertPointGuard guard(*resPtr->Builder);
        resPtr->Builder->SetInsertPoint(entry);
        resPtr->Builder->CreateRet(fn);
      }

      // Create IFunc: @mangled = ifunc <FnTy>, ptr @resolver
      llvm::GlobalIFunc::create(fn->getFunctionType(), 0,
                                llvm::GlobalValue::ExternalLinkage,
                                mangled, resolver, resPtr->Module.get());
    }
  }
}
void CgVisitor::postorder_walk(StructDefAST *ast) {}

void CgVisitor::postorder_walk(ReturnExprAST *ast) {
  // INFO: If we cannot parse return expr, treat it as unit for now
  if (ast->type == Type::Unit())
    resPtr->Builder->CreateRetVoid();
  else
    resPtr->Builder->CreateRet(ast->return_expr->val);
}

void CgVisitor::preorder_walk(BinaryExprAST *ast) {}

void CgVisitor::postorder_walk(BinaryExprAST *ast) {
  if (ast->Op->tok_type == TokenType::TokASSIGN) {
    auto *R = ast->RHS->val;
    if (!R)
      this->abort("Failed to codegen RHS for tok assign");

    if (auto *LHSE = dynamic_cast<VariableExprAST *>(ast->LHS.get())) {
      auto *Var = this->allocaValues.top()[LHSE->variableName];
      if (!Var)
        this->abort("Unknown variable in LHS of tok assign");
      resPtr->Builder->CreateStore(R, Var);
      ast->val = R;
      return;
    }

    if (auto *deref = dynamic_cast<DerefExprAST *>(ast->LHS.get())) {
      auto *ptr = deref->operand->val;
      if (!ptr)
        this->abort("Failed to codegen pointer for deref assignment");
      resPtr->Builder->CreateStore(R, ptr);
      ast->val = R;
      return;
    }

    if (auto *idx_expr = dynamic_cast<IndexExprAST *>(ast->LHS.get())) {
      // Handle (*ptr)[i] = val — assignment through dereferenced pointer
      if (auto *deref =
              dynamic_cast<DerefExprAST *>(idx_expr->array_expr.get())) {
        auto *ptr_val = deref->operand->val;
        auto &arr_data = std::get<ArrayType>(deref->type.type_data);
        auto *arr_llvm_type = type_converter.get_type(deref->type);
        auto *idx = idx_expr->index_expr->val;

        auto *gep = emitArrayElementGEP(ptr_val, idx, arr_llvm_type,
                                        arr_data.get_size());
        resPtr->Builder->CreateStore(R, gep);
        ast->val = R;
        return;
      }

      // Direct variable case: arr[i] = val
      auto *var_expr =
          dynamic_cast<VariableExprAST *>(idx_expr->array_expr.get());
      this->abort_if_not(var_expr,
                         "Array index assignment requires a variable");
      auto *alloca = this->allocaValues.top()[var_expr->variableName];
      this->abort_if_not(alloca, "Unknown array variable in index assignment");

      auto *arr_type = type_converter.get_type(var_expr->type);
      auto *idx = idx_expr->index_expr->val;
      auto &arr_data = std::get<ArrayType>(var_expr->type.type_data);
      auto *gep = emitArrayElementGEP(alloca, idx, arr_type,
                                      arr_data.get_size());
      resPtr->Builder->CreateStore(R, gep);
      ast->val = R;
      return;
    }

    this->abort("Left hand side of assignment must be a variable, "
                "dereferenced pointer, or array index");
    return;
  }
  auto *L = ast->LHS->val;
  auto *R = ast->RHS->val;
  auto &lhs_type = ast->LHS->type;
  bool is_int = lhs_type == Type::I32_t() || lhs_type == Type::I64_t();
  bool is_float = lhs_type == Type::F64_t();

  auto tok = ast->Op->tok_type;
  if (tok == TokenType::TokADD) {
    if (is_int)
      ast->val = resPtr->Builder->CreateAdd(L, R, "add_expr");
    else if (is_float)
      ast->val = resPtr->Builder->CreateFAdd(L, R, "add_expr");
    else
      this->abort();
  } else if (tok == TokenType::TokSUB) {
    if (is_int)
      ast->val = resPtr->Builder->CreateSub(L, R, "sub_expr");
    else if (is_float)
      ast->val = resPtr->Builder->CreateFSub(L, R, "sub_expr");
    else
      this->abort();
  } else if (tok == TokenType::TokMUL) {
    if (is_int)
      ast->val = resPtr->Builder->CreateMul(L, R, "mul_expr");
    else if (is_float)
      ast->val = resPtr->Builder->CreateFMul(L, R, "mul_expr");
    else
      this->abort();
  } else if (tok == TokenType::TokDIV) {
    if (is_int)
      ast->val = resPtr->Builder->CreateSDiv(L, R, "div_expr");
    else if (is_float)
      ast->val = resPtr->Builder->CreateFDiv(L, R, "div_expr");
    else
      this->abort();
  } else if (tok == TokOR) {
    ast->val = resPtr->Builder->CreateLogicalOr(L, R);
  } else if (tok == TokAND) {
    ast->val = resPtr->Builder->CreateLogicalAnd(L, R);
  } else if (ast->Op->is_comparison()) {
    if (lhs_type.type_kind == TypeKind::Array) {
      ast->val = emitArrayComparison(L, R, lhs_type, tok);
    } else {
      ast->val = resPtr->Builder->CreateCmp(
          type_converter.get_cmp_func(lhs_type, ast->RHS->type, tok), L, R);
    }
  } else if (tok == TokMOD) {
    ast->val = resPtr->Builder->CreateSRem(L, R);
  } else {
    LOG({ fmt::print(stderr, "{}\n", ast->Op->lexeme); });
    this->abort();
  }
}

void CgVisitor::preorder_walk(StringExprAST *ast) {}
void CgVisitor::preorder_walk(NumberExprAST *ast) {
  switch (ast->type.type_kind) {
  case TypeKind::I32_t:
    ast->val = llvm::ConstantInt::get(
        *resPtr->Context, llvm::APInt(32, std::stoi(ast->number), true));
    break;
  case TypeKind::I64_t:
    ast->val = llvm::ConstantInt::get(
        *resPtr->Context, llvm::APInt(64, std::stoll(ast->number), true));
    break;
  case TypeKind::F64_t:
    ast->val = llvm::ConstantFP::get(*resPtr->Context,
                                     llvm::APFloat(std::stod(ast->number)));
    break;
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::Function:
  case TypeKind::Pointer:
  case TypeKind::Array:
  case TypeKind::Never:
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::String:
  case TypeKind::Struct:
  case TypeKind::Integer:
  case TypeKind::Flt:
  case TypeKind::TypeParam:
    this->abort("NumberExprAST has invalid type kind");
  }
  this->abort_if_not(ast->val, "cannot generate number");
}
void CgVisitor::preorder_walk(BoolExprAST *ast) {
  ast->val = llvm::ConstantInt::get(llvm::Type::getInt1Ty(*resPtr->Context),
                                    ast->b ? 1 : 0);
}
void CgVisitor::preorder_walk(VariableExprAST *ast) {
  auto *alloca = this->allocaValues.top()[ast->variableName];

  if (alloca) {
    LOG({
      fmt::print(stderr,
                 "[codegen] VariableExprAST '{}': loaded from local alloca\n",
                 ast->variableName);
    });
    ast->val = resPtr->Builder->CreateLoad(alloca->getAllocatedType(), alloca,
                                           ast->variableName);
    return;
  }

  // Not a local variable — check if it's a module function used as a value
  auto *fn = resPtr->Module->getFunction(ast->variableName);
  if (fn && ast->type.type_kind == TypeKind::Function) {
    LOG({
      fmt::print(stderr,
                 "[codegen] VariableExprAST '{}': wrapping module function as "
                 "closure\n",
                 ast->variableName);
    });
    auto ft = std::get<FunctionType>(ast->type.type_data);
    auto *wrapper = getOrCreateClosureWrapper(fn, ft);
    auto *nullEnv = llvm::ConstantPointerNull::get(
        llvm::PointerType::get(*resPtr->Context, 0));
    ast->val = buildClosure(wrapper, nullEnv);
    return;
  }

  this->abort("Unknown variable name");
}
void CgVisitor::preorder_walk(UnitExprAST *ast) {}
void CgVisitor::preorder_walk(IfExprAST *ast) {}

// Override visit to control child traversal order for if-expr codegen
void CgVisitor::visit(IfExprAST *ast) {
  // First, codegen the condition
  ast->bool_expr->accept_vis(this);
  if (!ast->bool_expr->val) {
    this->abort("Failed to codegen condition of if-expr");
  }

  // Convert condition to i1 based on type
  switch (ast->bool_expr->type.type_kind) {
  case TypeKind::I32_t:
    ast->bool_expr->val = resPtr->Builder->CreateICmpNE(
        ast->bool_expr->val,
        llvm::ConstantInt::get(*resPtr->Context, llvm::APInt(32, 0)),
        "ifcond_i32");
    break;
  case TypeKind::I64_t:
    ast->bool_expr->val = resPtr->Builder->CreateICmpNE(
        ast->bool_expr->val,
        llvm::ConstantInt::get(*resPtr->Context, llvm::APInt(64, 0)),
        "ifcond_i64");
    break;
  case TypeKind::F64_t:
    ast->bool_expr->val = resPtr->Builder->CreateFCmpONE(
        ast->bool_expr->val,
        llvm::ConstantFP::get(*resPtr->Context, llvm::APFloat(0.0)),
        "ifcond_f64");
    break;
  case TypeKind::NonExistent:
  case TypeKind::Poisoned:
  case TypeKind::Unit:
  case TypeKind::Struct:
  case TypeKind::Array:
  case TypeKind::Function:
  case TypeKind::Pointer:
  case TypeKind::Never:
  case TypeKind::String:
  case TypeKind::Integer:
  case TypeKind::Flt:
  case TypeKind::TypeParam:
    LOG({
      fmt::print(stderr, "[CODEGEN] Logging IfExprAST's bool expr \n");
      ASTPrinter::print(ast->bool_expr.get());
    });
    this->abort("Invalid syntax or broken typechecker for now\n");
    break;
  case TypeKind::Bool:
    // Bool is already i1, usable directly by CreateCondBr
    break;
  }

  llvm::Function *function = resPtr->Builder->GetInsertBlock()->getParent();

  // Create blocks for the then and else cases
  BasicBlock *ThenBB = BasicBlock::Create(*resPtr->Context, "then", function);
  BasicBlock *ElseBB = BasicBlock::Create(*resPtr->Context, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*resPtr->Context, "ifcont");

  resPtr->Builder->CreateCondBr(ast->bool_expr->val, ThenBB, ElseBB);

  auto blockBranchHelper = [this, &MergeBB](BasicBlock *bb,
                                            BlockAST *block_ast) {
    resPtr->Builder->SetInsertPoint(bb);
    block_ast->accept_vis(this);

    llvm::Value *V = nullptr;
    if (!block_ast->Statements.empty()) {
      V = block_ast->Statements.back()->val;
    }
    BasicBlock *end_bb = resPtr->Builder->GetInsertBlock();
    // Get the actual block we're in now (codegen might have created new blocks)
    if (!end_bb->getTerminator())
      resPtr->Builder->CreateBr(MergeBB);

    return std::pair{V, end_bb};
  };

  auto [ThenV, ThenEndBB] = blockBranchHelper(ThenBB, ast->thenBlockAST.get());

  // Codegen else block
  function->insert(function->end(), ElseBB);

  auto [ElseV, ElseEndBB] = blockBranchHelper(ElseBB, ast->elseBlockAST.get());

  // Continue at merge block only if it has predecessors
  if (MergeBB->hasNPredecessorsOrMore(1)) {
    function->insert(function->end(), MergeBB);
    resPtr->Builder->SetInsertPoint(MergeBB);

    // Create PHI node to merge values if both branches produce values
    // and the if expression has a non-Unit, non-Never type
    if (ThenV && ElseV && ast->type.type_kind != TypeKind::Unit &&
        ast->type.type_kind != TypeKind::Never) {
      llvm::PHINode *PN =
          resPtr->Builder->CreatePHI(ThenV->getType(), 2, "iftmp");
      PN->addIncoming(ThenV, ThenEndBB);
      PN->addIncoming(ElseV, ElseEndBB);
      ast->val = PN;
    }
  } else {
    // Both branches terminated, no merge block needed
    delete MergeBB;
  }
}
void CgVisitor::preorder_walk(TypedVarAST *ast) {}

void CgVisitor::postorder_walk(StringExprAST *ast) {
  ast->val = resPtr->Builder->CreateGlobalString(ast->string_content);
  ast->type = Type::String(ast->string_content);
}

void CgVisitor::visit(DerefExprAST *ast) {
  ast->walk_with_preorder(this);
  ast->operand->accept_vis(this);
  ast->walk_with_postorder(this);
}

void CgVisitor::postorder_walk(DerefExprAST *ast) {
  auto pointee_type =
      std::get<PointerType>(ast->operand->type.type_data).get_pointee();
  ast->val = resPtr->Builder->CreateLoad(type_converter.get_type(pointee_type),
                                         ast->operand->val, "deref");
}

void CgVisitor::postorder_walk(AllocExprAST *ast) {
  // Extract element type T from ptr<T>
  auto pointee_type =
      std::get<PointerType>(ast->type.type_data).get_pointee();
  auto *elem_llvm_type = type_converter.get_type(pointee_type);

  // Compute sizeof(T) using DataLayout
  auto &data_layout = resPtr->Module->getDataLayout();
  auto elem_size = data_layout.getTypeAllocSize(elem_llvm_type);
  auto *elem_size_val =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(*resPtr->Context), elem_size);

  // Compute total size = sizeof(T) * count
  auto *count_val = ast->operand->val;
  // Extend count to i64 if it's i32
  if (count_val->getType()->isIntegerTy(32))
    count_val = resPtr->Builder->CreateZExt(
        count_val, llvm::Type::getInt64Ty(*resPtr->Context), "count_ext");
  auto *total_size =
      resPtr->Builder->CreateMul(elem_size_val, count_val, "alloc_size");

  // Call malloc(total_size)
  auto *malloc_fn = resPtr->Module->getFunction("malloc");
  auto *malloc_ptr =
      resPtr->Builder->CreateCall(malloc_fn, {total_size}, "alloc_ptr");

  ast->val = malloc_ptr;
}

void CgVisitor::postorder_walk(FreeExprAST *ast) {
  auto *ptr_val = ast->operand->val;

  // Call free(ptr)
  auto *free_fn = resPtr->Module->getFunction("free");
  resPtr->Builder->CreateCall(free_fn, {ptr_val});

  ast->val = nullptr;
}

void CgVisitor::visit(AddrOfExprAST *ast) {
  ast->walk_with_preorder(this);
  // Don't visit operand normally - we need the alloca, not the loaded value
  auto *var_expr = dynamic_cast<VariableExprAST *>(ast->operand.get());
  this->abort_if_not(var_expr, "Address-of (&) requires a variable operand");
  auto *alloca = this->allocaValues.top()[var_expr->variableName];
  this->abort_if_not(alloca, "Unknown variable in address-of expression");
  ast->val = alloca;
  ast->walk_with_postorder(this);
}

// Taken from
// echo 'void f(int *out) { int a[] = {10,20,30}; }' | clang -xc - -emit-llvm -S
// -o -
void CgVisitor::postorder_walk(ArrayLiteralExprAST *ast) {
  auto *arr_llvm_type =
      llvm::cast<llvm::ArrayType>(type_converter.get_type(ast->type));

  // Collect element constants
  std::vector<llvm::Constant *> constants;
  for (auto &elem : ast->elements) {
    auto *c = llvm::dyn_cast<llvm::Constant>(elem->val);
    this->abort_if_not(c, "Array literal elements must be constants");
    constants.push_back(c);
  }

  // Build a global constant array and memcpy into a local alloca
  auto *const_arr = llvm::ConstantArray::get(arr_llvm_type, constants);
  auto *global = new llvm::GlobalVariable(
      *resPtr->Module, arr_llvm_type, /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, const_arr, ".arr_literal");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  auto *alloca = CodegenUtils::CreateEntryBlockAlloca(
      getCurrentFunction(), "arr_lit_tmp", arr_llvm_type);

  auto &data_layout = resPtr->Module->getDataLayout();
  auto size = data_layout.getTypeAllocSize(arr_llvm_type);

  resPtr->Builder->CreateMemCpy(alloca, alloca->getAlign(), global,
                                global->getAlign(), size);

  ast->val = resPtr->Builder->CreateLoad(arr_llvm_type, alloca, "arr_val");
}

void CgVisitor::visit(IndexExprAST *ast) {
  // Don't visit array_expr normally — we need the alloca, not the loaded value
  ast->walk_with_preorder(this);
  // If array_expr is (*ptr), visit only the operand to get the pointer value.
  // We skip the deref itself — we don't want to load the whole array,
  // we just need the pointer so postorder_walk can GEP into it.
  if (auto *deref = dynamic_cast<DerefExprAST *>(ast->array_expr.get())) {
    deref->operand->accept_vis(this);
  }
  // Only visit index_expr to get the index value
  ast->index_expr->accept_vis(this);
  ast->walk_with_postorder(this);
}

void CgVisitor::postorder_walk(IndexExprAST *ast) {
  // Handle (*ptr)[i] — array indexing through dereferenced pointer
  if (auto *deref = dynamic_cast<DerefExprAST *>(ast->array_expr.get())) {
    auto *ptr_val =
        deref->operand->val; // pointer to the array (loaded by visit())
    // deref->type is the pointee type = the array type
    auto &arr_data = std::get<ArrayType>(deref->type.type_data);
    auto *arr_llvm_type = type_converter.get_type(deref->type);
    auto *idx = ast->index_expr->val;

    auto *gep = emitArrayElementGEP(ptr_val, idx, arr_llvm_type,
                                    arr_data.get_size());
    ast->val = resPtr->Builder->CreateLoad(
        type_converter.get_type(arr_data.get_element()), gep, "arr_elem");
    return;
  }

  // Direct variable case: arr[i]
  auto *var_expr = dynamic_cast<VariableExprAST *>(ast->array_expr.get());
  this->abort_if_not(var_expr, "Array indexing requires a variable");
  auto *alloca = this->allocaValues.top()[var_expr->variableName];
  this->abort_if_not(alloca, "Unknown array variable");

  auto &arr_data = std::get<ArrayType>(var_expr->type.type_data);
  auto *arr_type = type_converter.get_type(var_expr->type);
  auto *idx = ast->index_expr->val;

  auto *gep = emitArrayElementGEP(alloca, idx, arr_type, arr_data.get_size());
  ast->val = resPtr->Builder->CreateLoad(
      type_converter.get_type(arr_data.get_element()), gep, "arr_elem");
}

void CgVisitor::postorder_walk(LenExprAST *ast) {
  auto arr_size = std::get<ArrayType>(ast->operand->type.type_data).get_size();
  ast->val = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*resPtr->Context),
                                    arr_size);
}

void CgVisitor::emitBoundsCheck(llvm::Value *idx, size_t arr_size) {
  auto &Ctx = *resPtr->Context;
  auto *i32Ty = llvm::Type::getInt32Ty(Ctx);
  auto *size_const = llvm::ConstantInt::get(i32Ty, arr_size);
  auto *zero = llvm::ConstantInt::get(i32Ty, 0);

  auto *oob_high = resPtr->Builder->CreateICmpSGE(idx, size_const, "oob_high");
  auto *oob_low = resPtr->Builder->CreateICmpSLT(idx, zero, "oob_low");
  auto *oob = resPtr->Builder->CreateOr(oob_high, oob_low, "oob");

  auto *function = resPtr->Builder->GetInsertBlock()->getParent();
  auto *ErrorBB = BasicBlock::Create(Ctx, "oob_error", function);
  auto *ContinueBB = BasicBlock::Create(Ctx, "oob_ok", function);

  resPtr->Builder->CreateCondBr(oob, ErrorBB, ContinueBB);

  resPtr->Builder->SetInsertPoint(ErrorBB);
  auto *printf_fn = resPtr->Module->getFunction("printf");
  auto *err_msg = resPtr->Builder->CreateGlobalString(
      "Array index out of bounds\n", "oob_msg");
  resPtr->Builder->CreateCall(printf_fn, {err_msg});
  auto *exit_fn = resPtr->Module->getFunction("exit");
  resPtr->Builder->CreateCall(exit_fn, {llvm::ConstantInt::get(i32Ty, 1)});
  resPtr->Builder->CreateUnreachable();

  resPtr->Builder->SetInsertPoint(ContinueBB);
}

void CgVisitor::postorder_walk(UnaryNegExprAST *ast) {
  auto *operand_val = ast->operand->val;
  if (ast->operand->type == Type::I32_t() ||
      ast->operand->type == Type::I64_t())
    ast->val = resPtr->Builder->CreateNeg(operand_val, "neg");
  else if (ast->operand->type == Type::F64_t())
    ast->val = resPtr->Builder->CreateFNeg(operand_val, "fneg");
  else
    this->abort("UnaryNegExprAST has invalid operand type");
}

void CgVisitor::postorder_walk(StructLiteralExprAST *ast) {
  if (ast->type.type_kind == TypeKind::Poisoned)
    return;

  auto &st = std::get<StructType>(ast->type.type_data);
  auto *llvm_struct_ty = type_converter.get_struct_type(st.get_name());

  llvm::Value *agg = llvm::UndefValue::get(llvm_struct_ty);
  for (size_t i = 0; i < ast->field_values.size(); i++) {
    auto field_idx = st.get_field_index(ast->field_names[i]);
    agg = resPtr->Builder->CreateInsertValue(agg, ast->field_values[i]->val,
                                             {(unsigned)field_idx.value()});
  }
  ast->val = agg;
}

void CgVisitor::postorder_walk(FieldAccessExprAST *ast) {
  if (ast->type.type_kind == TypeKind::Poisoned)
    return;

  auto obj_type = ast->object_expr->type;
  auto &st = std::get<StructType>(obj_type.type_data);
  auto field_idx = st.get_field_index(ast->field_name);

  ast->val = resPtr->Builder->CreateExtractValue(
      ast->object_expr->val, {(unsigned)field_idx.value()},
      ast->field_name);
}

llvm::Value *CgVisitor::emitArrayComparison(llvm::Value *L, llvm::Value *R,
                                             const Type &arrType,
                                             TokenType tok) {
  auto &Ctx = *resPtr->Context;
  auto &arr_data = std::get<ArrayType>(arrType.type_data);
  auto arr_size = arr_data.get_size();
  auto elem_type = arr_data.get_element();
  auto *arr_llvm_type =
      llvm::cast<llvm::ArrayType>(type_converter.get_type(arrType));

  auto *function = resPtr->Builder->GetInsertBlock()->getParent();

  // Store L and R into temporary allocas so we can GEP into them
  auto *alloca_l = CodegenUtils::CreateEntryBlockAlloca(function, "arr_cmp_l",
                                                        arr_llvm_type);
  auto *alloca_r = CodegenUtils::CreateEntryBlockAlloca(function, "arr_cmp_r",
                                                        arr_llvm_type);
  resPtr->Builder->CreateStore(L, alloca_l);
  resPtr->Builder->CreateStore(R, alloca_r);

  auto *i32Ty = llvm::Type::getInt32Ty(Ctx);
  auto *i1Ty = llvm::Type::getInt1Ty(Ctx);
  auto *zero = llvm::ConstantInt::get(i32Ty, 0);
  auto *size_val = llvm::ConstantInt::get(i32Ty, arr_size);
  auto *true_val = llvm::ConstantInt::getTrue(Ctx);
  auto *false_val = llvm::ConstantInt::getFalse(Ctx);

  // Basic blocks:
  //   entry -> loop_header -> loop_body -> loop_latch -> loop_header
  //                |                 |
  //                v (all equal)     v (mismatch)
  //            loop_exit <------ loop_mismatch
  auto *entry_bb = resPtr->Builder->GetInsertBlock();
  auto *loop_header = BasicBlock::Create(Ctx, "arr_cmp_header", function);
  auto *loop_body = BasicBlock::Create(Ctx, "arr_cmp_body", function);
  auto *loop_latch = BasicBlock::Create(Ctx, "arr_cmp_latch", function);
  auto *loop_mismatch = BasicBlock::Create(Ctx, "arr_cmp_mismatch", function);
  auto *loop_exit = BasicBlock::Create(Ctx, "arr_cmp_exit", function);

  resPtr->Builder->CreateBr(loop_header);

  // Loop header: counter phi, bounds check
  resPtr->Builder->SetInsertPoint(loop_header);
  auto *counter = resPtr->Builder->CreatePHI(i32Ty, 2, "arr_cmp_i");
  counter->addIncoming(zero, entry_bb);
  auto *cond =
      resPtr->Builder->CreateICmpSLT(counter, size_val, "arr_cmp_cond");
  resPtr->Builder->CreateCondBr(cond, loop_body, loop_exit);

  // Loop body: load elements, compare
  resPtr->Builder->SetInsertPoint(loop_body);
  auto *elem_llvm_type = type_converter.get_type(elem_type);
  auto *gep_l = resPtr->Builder->CreateGEP(arr_llvm_type, alloca_l,
                                            {zero, counter}, "arr_cmp_gep_l");
  auto *gep_r = resPtr->Builder->CreateGEP(arr_llvm_type, alloca_r,
                                            {zero, counter}, "arr_cmp_gep_r");
  auto *elem_l =
      resPtr->Builder->CreateLoad(elem_llvm_type, gep_l, "arr_cmp_el");
  auto *elem_r =
      resPtr->Builder->CreateLoad(elem_llvm_type, gep_r, "arr_cmp_er");

  llvm::Value *elem_eq;
  if (elem_type.type_kind == TypeKind::Array) {
    elem_eq = emitArrayComparison(elem_l, elem_r, elem_type, TokEQUAL);
  } else {
    elem_eq = resPtr->Builder->CreateCmp(
        type_converter.get_cmp_func(elem_type, elem_type, TokEQUAL), elem_l,
        elem_r, "arr_cmp_eq");
  }

  // Short-circuit: if elements differ, jump to mismatch
  resPtr->Builder->CreateCondBr(elem_eq, loop_latch, loop_mismatch);

  // Loop latch: increment counter, branch back to header
  resPtr->Builder->SetInsertPoint(loop_latch);
  auto *next_i = resPtr->Builder->CreateAdd(
      counter, llvm::ConstantInt::get(i32Ty, 1), "arr_cmp_next");
  counter->addIncoming(next_i, loop_latch);
  resPtr->Builder->CreateBr(loop_header);

  // Mismatch exit: arrays differ
  resPtr->Builder->SetInsertPoint(loop_mismatch);
  resPtr->Builder->CreateBr(loop_exit);

  // Final exit: phi merges true (all matched) vs false (mismatch)
  resPtr->Builder->SetInsertPoint(loop_exit);
  auto *result = resPtr->Builder->CreatePHI(i1Ty, 2, "arr_cmp_result");
  result->addIncoming(true_val, loop_header);
  result->addIncoming(false_val, loop_mismatch);

  // For !=, negate the == result
  if (tok == TokNOTEqual) {
    return resPtr->Builder->CreateNot(result, "arr_cmp_ne");
  }
  return result;
}

llvm::Value *CgVisitor::emitArrayElementGEP(llvm::Value *base,
                                             llvm::Value *idx,
                                             llvm::Type *arrLlvmType,
                                             size_t arrSize) {
  emitBoundsCheck(idx, arrSize);
  return resPtr->Builder->CreateGEP(
      arrLlvmType, base,
      {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*resPtr->Context), 0),
       idx},
      "arr_idx");
}

llvm::Value *CgVisitor::buildClosure(llvm::Value *codePtr,
                                     llvm::Value *envPtr) {
  auto *closureTy =
      llvm::StructType::getTypeByName(*resPtr->Context, "sammine.closure");
  llvm::Value *closure = llvm::UndefValue::get(closureTy);
  closure =
      resPtr->Builder->CreateInsertValue(closure, codePtr, 0, "cls.code");
  closure =
      resPtr->Builder->CreateInsertValue(closure, envPtr, 1, "cls.env");
  return closure;
}

} // namespace sammine_lang::AST
