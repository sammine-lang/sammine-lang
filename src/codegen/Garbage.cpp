#include "ast/Ast.h"
#include "codegen/CodegenUtils.h"
#include "fmt/format.h"
#include "util/Utilities.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include <codegen/Garbage.h>
#include <llvm/ADT/APInt.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Value.h>
#include <memory>

namespace sammine_lang::AST {
/// Insert a FrameMap in the beginning of each function
llvm::GlobalVariable *
ShadowGarbageCollector::getFrameMapForCallee(FuncDefAST *callee) {
  std::string frame_map_name = fmt::format(
      "{}_{}", std::string(callee->Prototype->functionName), FRAME_MAP);
  if (auto fm = module.getGlobalVariable(frame_map_name))
    return fm;
  /// The map for a single function's stack frame.  One of these is
  ///        compiled as constant data into the executable for each function.
  /// INFO: from Jasmine: Ok, this is set up so that it scales into a future
  /// where we not only stores the NumRoots but also the meta data for each
  /// root.
  ///
  /// INFO: Right now, we only have NumRoots so it might seem wasteful but in a
  /// far future, this will be more efficient. I just don't want to incorporate
  /// NumRoots straight into stack entry now so later i have to rewrite all this
  ///
  /// Storage of metadata values is elided if the %metadata parameter to
  /// @llvm.gcroot is null.

  // struct FrameMap {
  //   (0). int32_t NumRoots;    //< Number of roots in stack frame.
  //   (1). int32_t NumMeta;     //< Number of metadata entries.  May be <
  //   (2). const void ShadowGarbageCollector:: *Meta[0]; //< Metadata
  //   for each root.
  auto llvm_num_roots =
      llvm::APInt(64, NumRootCalculator::calculate(callee->Block.get()), true);

  llvm::Constant *frame_map_init = llvm::ConstantStruct::get(
      FRAME_MAP_TYPE, llvm::ConstantInt::get(context, llvm_num_roots));
  // INFO: new-allocated. This is fine since module will be managing the pointer
  // for us
  return new llvm::GlobalVariable(module, FRAME_MAP_TYPE, true,
                                  llvm::GlobalValue::ExternalLinkage,
                                  frame_map_init, frame_map_name);
}
void ShadowGarbageCollector::relieveStackEntry() {
  // allocate a struct to store the current global root chain info

  auto *global_root_chain = module.getGlobalVariable(GLOBAL_ROOT_CHAIN);
  // load it into a stack variable
  auto caller_stack_entry = builder.CreateLoad(
      STACK_ENTRY_TYPE,
      builder.CreateStructGEP(STACK_ENTRY_TYPE, global_root_chain, 0),
      "caller_stack_entry");

  builder.CreateStore(caller_stack_entry, global_root_chain);
}
void ShadowGarbageCollector::applyStrategy(llvm::Function *f) {
  f->setGC(this->llvmStrategy());
}
void ShadowGarbageCollector::setStackEntry(FuncDefAST *callee,
                                           llvm::Function *llvm_callee) {

  /// INFO: We'll set the global curr_stack_entry->next to be the caller,
  /// We'll also set the curr_stack_entry->frame_map to be the callee's frame
  /// map
  ///
  /// INFO: this will require us to keep a map of string to FrameMap in the
  /// ShadowGarbageCollector class.
  /// For more details, see createFrameMap
  ///
  ///
  auto *callee_stack_entry = CodegenUtils::CreateEntryBlockAlloca(
      llvm_callee, "stack_entry", STACK_ENTRY_TYPE);

  // 1. Get global_root_chain
  llvm::GlobalVariable *globalRootChain =
      module.getNamedGlobal(GLOBAL_ROOT_CHAIN);
  assert(globalRootChain && "global_root_chain not defined");

  // 2. store the address of global root chain to the first element of of
  // callee_stack_entry (shadow stack)
  // INFO: Only store this if this is not the entry point, a.k.a the main
  // function
  if (!CodegenUtils::isFunctionMain(callee)) {
    llvm::Value *nextFieldPtr = builder.CreateStructGEP(
        STACK_ENTRY_TYPE, callee_stack_entry, 0, "next_field_ptr");
    builder.CreateStore(globalRootChain, nextFieldPtr, "old_head");
  }

  // 3. store the address of global frame map to the second element of
  // callee_stack_entry

  llvm::Value *frameMapPtr = builder.CreateStructGEP(
      STACK_ENTRY_TYPE, callee_stack_entry, 1, "framemap_field_ptr");
  auto frame_map = this->getFrameMapForCallee(callee);
  builder.CreateStore(frame_map, frameMapPtr);

  // Make the current stack entry to be the global_root_chain
  // globalRootChain->setInitialize
  builder.CreateStore(callee_stack_entry, globalRootChain);
}

void ShadowGarbageCollector::initGlobalRootChain() {
  /// The head of the singly-linked list of StackEntries.  Functions push
  ///        and pop onto this in their prologue and epilogue.
  ///
  /// Since there is only a global list, this technique is not threadsafe.
  // StackEntry *llvm_gc_root_chain;
  // INFO: Create a stack entry

  // INFO: Create a struct constant so we can use it to initialize the global
  // root chain
  auto *null_stack_entry = STACK_ENTRY_TYPE->getElementType(0); // StackEntry*
  auto *null_frame_map = STACK_ENTRY_TYPE->getElementType(1);   //
  auto *root_arr = STACK_ENTRY_TYPE->getElementType(2);         //

  auto null_entry_ptr = llvm::ConstantPointerNull::get(
      llvm::cast<llvm::PointerType>(null_stack_entry));
  auto null_frame_map_ptr = llvm::ConstantPointerNull::get(
      llvm::cast<llvm::PointerType>(null_frame_map));

  auto root_arr_init =
      llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(root_arr), {});
  llvm::Constant *stack_entry_initializer = llvm::ConstantStruct::get(
      STACK_ENTRY_TYPE, {null_entry_ptr, null_frame_map_ptr, root_arr_init}
      // ignore Roots[0] since it's unsized
  );

  // INFO: Put it in the module as a global root
  //
  // INFO: Don't worry about a raw new here, the module takes ownership of it,
  //
  // INFO: We use new here and not unique ptr because we don't own it.
  //      We don't use local stack variable because if we exit this scope, we
  //      lose the global variables handled by the module
  //      We don't use shared ptr because we need the module to be the one
  //      decomissioning it
  //
  // INFO: new-allocated
  new llvm::GlobalVariable(module, STACK_ENTRY_TYPE,
                           /* isConstant*/ false,
                           llvm::GlobalValue::ExternalLinkage,
                           stack_entry_initializer, GLOBAL_ROOT_CHAIN);
}

void ShadowGarbageCollector::initGCFunc() {
  /// Calls Visitor(root, meta) for each GC root on the stack.
  ///        root and meta are exactly the values passed to
  ///        @llvm.gcroot.
  ///
  /// Visitor could be a function to recursively mark live objects.  Or it
  /// might copy them to another heap or generation.
  ///
  /// @param Visitor A function to invoke for every GC root on the stack.
  // void ShadowGarbageCollector:: visitGCRoots(void (*Visitor)(void **Root,
  // const void *Meta)) {
  //   for (StackEntry *R = llvm_gc_root_chain; R; R = R->Next) {
  //     unsigned i = 0;
  //
  //     // For roots [0, NumMeta), the metadata pointer is in the FrameMap.
  //     for (unsigned e = R->Map->NumMeta; i != e; ++i)
  //       Visitor(&R->Roots[i], R->Map->Meta[i]);
  //
  //     // For roots [NumMeta, NumRoots), the metadata pointer is null.
  //     for (unsigned e = R->Map->NumRoots; i != e; ++i)
  //       Visitor(&R->Roots[i], NULL);
  //   }
}

int32_t NumRootCalculator::calculate(BlockAST *ast) {
  int num_roots = 0;
  for (auto &e : ast->Statements) {
    num_roots += calculate(e.get());
  }

  return num_roots;
}
int32_t NumRootCalculator::calculate(ExprAST *ast) {
  if (auto e = dynamic_cast<IfExprAST *>(ast)) {
    return calculate(e);
  } else if (auto e = dynamic_cast<VariableExprAST *>(ast)) {
    return calculate(e);
  } else if (auto e = dynamic_cast<CallExprAST *>(ast)) {
    return calculate(e);
  } else if (auto e = dynamic_cast<ReturnExprAST *>(ast)) {
    return calculate(e);
  } else if (auto e = dynamic_cast<BoolExprAST *>(ast)) {
    return calculate(e);
  } else if (auto e = dynamic_cast<StringExprAST *>(ast)) {
    return calculate(e);
  } else if (auto e = dynamic_cast<NumberExprAST *>(ast)) {
    return calculate(e);
  } else if (auto e = dynamic_cast<VarDefAST *>(ast)) {
    return calculate(e);
  } else if (auto e = dynamic_cast<UnitExprAST *>(ast)) {
    return calculate(e);
  } else {
    sammine_util::abort(
        fmt::format("You should be overloading on {}", ast->getTreeName()));
    return 0;
  }
}
int32_t NumRootCalculator::calculate(IfExprAST *ast) {
  return calculate(ast->bool_expr.get()) + calculate(ast->thenBlockAST.get()) +
         calculate(ast->elseBlockAST.get());
}
int32_t NumRootCalculator::calculate(VariableExprAST *ast) { return 0; }

// TODO: Tell Jasmine to re-check this
int32_t NumRootCalculator::calculate(CallExprAST *ast) {
  int32_t num_roots = 0;
  for (auto &arg : ast->arguments) {
    num_roots += calculate(arg.get());
  }
  return num_roots;
}

int32_t NumRootCalculator::calculate(ReturnExprAST *ast) {
  return calculate(ast->return_expr.get());
}
int32_t NumRootCalculator::calculate(BinaryExprAST *ast) {
  auto left = calculate(ast->LHS.get());
  auto right = calculate(ast->RHS.get());
  return left + right;
}
int32_t NumRootCalculator::calculate(BoolExprAST *ast) { return 0; }
int32_t NumRootCalculator::calculate(StringExprAST *ast) { return 1; }
int32_t NumRootCalculator::calculate(NumberExprAST *ast) { return 0; }
int32_t NumRootCalculator::calculate(UnitExprAST *ast) { return 0; }
int32_t NumRootCalculator::calculate(VarDefAST *ast) {
  return calculate(ast->Expression.get());
}

// We'll generate different code depending on if its a main file or not
void RefCounter::declare_malloc_wrapper(bool is_main_file) {
  auto malloc_type = CodegenUtils::declare_malloc(module);

  // Same type as malloc
  module.getOrInsertFunction(REFCNT_MALLOC_WRAPPER_NAME, malloc_type);
  // Check if refcnt_malloc_wrapper has been declared or not
  // -----MAIN FILE---- only
  // define refcnt_malloc_wrapper

  if (is_main_file) {
    // Function already declared earlier:
    llvm::Function *WrapperFunc =
        module.getFunction(REFCNT_MALLOC_WRAPPER_NAME);

    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(context, "entry", WrapperFunc);
    llvm::IRBuilder<> Builder(entry);

    // Get function argument (size)
    llvm::Argument *sizeArg = WrapperFunc->getArg(0);
    sizeArg->setName("size");

    // Add sizeof(i32) to size (i64)
    llvm::Value *refcntSize = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(context), sizeof(int32_t)); // usually 4
    llvm::Value *totalSize =
        Builder.CreateAdd(sizeArg, refcntSize, "total_size");

    // Call malloc(totalSize)
    llvm::Function *MallocFunc = module.getFunction("malloc");
    llvm::Value *rawPtr =
        Builder.CreateCall(MallocFunc, {totalSize}, "raw_mem");

    // Cast to i32* to store refcnt at start
    llvm::Type *i32Ty = llvm::Type::getInt32Ty(context);
    llvm::PointerType *i32PtrTy = llvm::PointerType::get(context, 0);
    llvm::Value *refcntPtr =
        Builder.CreateBitCast(rawPtr, i32PtrTy, "refcnt_ptr");

    // Store refcnt = 1
    Builder.CreateStore(llvm::ConstantInt::get(i32Ty, 1), refcntPtr);

    // Return data pointer (i8*)
    Builder.CreateRet(rawPtr);
  }
}

void RefCounter::increase_refcnt(llvm::Value *ptr) {
  if (!ptr) return;
  
  // Cast pointer to i8* if needed
  llvm::Type *i8Ty = llvm::Type::getInt8Ty(context);
  llvm::PointerType *i8PtrTy = llvm::PointerType::get(context, 0);
  llvm::Value *i8Ptr = builder.CreateBitCast(ptr, i8PtrTy, "ptr_as_i8");
  
  // Get pointer to ref count (4 bytes before the data)
  llvm::Type *i32Ty = llvm::Type::getInt32Ty(context);
  llvm::Value *refcntOffset = llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(context), -static_cast<int64_t>(REFCNT_SIZE));
  llvm::Value *refcntPtr = builder.CreateGEP(i8Ty, i8Ptr, refcntOffset, "refcnt_ptr");
  
  // Cast to i32* and load current ref count
  llvm::PointerType *i32PtrTy = llvm::PointerType::get(context, 0);
  llvm::Value *refcntI32Ptr = builder.CreateBitCast(refcntPtr, i32PtrTy, "refcnt_i32_ptr");
  llvm::Value *currentRefcnt = builder.CreateLoad(i32Ty, refcntI32Ptr, "current_refcnt");
  
  // Increment and store
  llvm::Value *newRefcnt = builder.CreateAdd(
      currentRefcnt, llvm::ConstantInt::get(i32Ty, 1), "new_refcnt");
  builder.CreateStore(newRefcnt, refcntI32Ptr);
}

void RefCounter::decrease_refcnt(llvm::Value *ptr) {
  if (!ptr) return;
  
  // Cast pointer to i8* if needed
  llvm::Type *i8Ty = llvm::Type::getInt8Ty(context);
  llvm::PointerType *i8PtrTy = llvm::PointerType::get(context, 0);
  llvm::Value *i8Ptr = builder.CreateBitCast(ptr, i8PtrTy, "ptr_as_i8");
  
  // Get pointer to ref count (4 bytes before the data)
  llvm::Type *i32Ty = llvm::Type::getInt32Ty(context);
  llvm::Value *refcntOffset = llvm::ConstantInt::get(
      llvm::Type::getInt64Ty(context), -static_cast<int64_t>(REFCNT_SIZE));
  llvm::Value *refcntPtr = builder.CreateGEP(i8Ty, i8Ptr, refcntOffset, "refcnt_ptr");
  
  // Cast to i32* and load current ref count
  llvm::PointerType *i32PtrTy = llvm::PointerType::get(context, 0);
  llvm::Value *refcntI32Ptr = builder.CreateBitCast(refcntPtr, i32PtrTy, "refcnt_i32_ptr");
  llvm::Value *currentRefcnt = builder.CreateLoad(i32Ty, refcntI32Ptr, "current_refcnt");
  
  // Decrement ref count
  llvm::Value *newRefcnt = builder.CreateSub(
      currentRefcnt, llvm::ConstantInt::get(i32Ty, 1), "new_refcnt");
  builder.CreateStore(newRefcnt, refcntI32Ptr);
  
  // Check if ref count reached zero
  llvm::Value *isZero = builder.CreateICmpEQ(
      newRefcnt, llvm::ConstantInt::get(i32Ty, 0), "is_zero");
  
  // Create basic blocks for conditional free
  llvm::Function *currentFunc = builder.GetInsertBlock()->getParent();
  llvm::BasicBlock *freeBB = llvm::BasicBlock::Create(context, "free_block", currentFunc);
  llvm::BasicBlock *continueBB = llvm::BasicBlock::Create(context, "continue_block", currentFunc);
  
  // Branch based on ref count
  builder.CreateCondBr(isZero, freeBB, continueBB);
  
  // Free block - call free() on the original allocated pointer (with ref count)
  builder.SetInsertPoint(freeBB);
  llvm::Function *freeFunc = module.getFunction("free");
  if (!freeFunc) {
    // Declare free function if not already declared
    llvm::FunctionType *freeType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {llvm::PointerType::get(context, 0)}, false);
    freeFunc = llvm::Function::Create(freeType, llvm::Function::ExternalLinkage, 
                                     "free", &module);
  }
  builder.CreateCall(freeFunc, {refcntPtr});
  builder.CreateBr(continueBB);
  
  // Continue block
  builder.SetInsertPoint(continueBB);
}
void RefCounter::declare_refcnt_visitor(bool is_main_file,
                                        llvm::StructType *stack_entry_type,
                                        llvm::StructType *frame_map_type) {
  // Declare the refcnt_visitor function type: void refcnt_visitor(void)
  llvm::FunctionType *visitorType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context), {}, false);
  
  llvm::Function *visitorFunc = llvm::Function::Create(
      visitorType, llvm::Function::ExternalLinkage, "refcnt_visitor", &module);
  
  if (is_main_file) {
    // Define the function body only in main file
    llvm::BasicBlock *entryBB = llvm::BasicBlock::Create(context, "entry", visitorFunc);
    llvm::IRBuilder<> visitorBuilder(entryBB);
    
    // Get types we'll need
    llvm::Type *i32Ty = llvm::Type::getInt32Ty(context);
    llvm::Type *i64Ty = llvm::Type::getInt64Ty(context);
    llvm::PointerType *i8PtrTy = llvm::PointerType::get(context, 0);
    llvm::PointerType *stackEntryPtrTy = llvm::PointerType::get(context, 0);
    llvm::PointerType *frameMapPtrTy = llvm::PointerType::get(context, 0);
    
    // Get global_root_chain
    llvm::GlobalVariable *globalRootChain = module.getGlobalVariable("global_root_chain");
    assert(globalRootChain && "global_root_chain not found");
    
    // Load current stack entry
    llvm::Value *currentEntry = visitorBuilder.CreateLoad(
        stackEntryPtrTy, globalRootChain, "current_entry");
    
    // Create loop blocks
    llvm::BasicBlock *loopCondBB = llvm::BasicBlock::Create(context, "loop_cond", visitorFunc);
    llvm::BasicBlock *loopBodyBB = llvm::BasicBlock::Create(context, "loop_body", visitorFunc);
    llvm::BasicBlock *innerLoopCondBB = llvm::BasicBlock::Create(context, "inner_loop_cond", visitorFunc);
    llvm::BasicBlock *innerLoopBodyBB = llvm::BasicBlock::Create(context, "inner_loop_body", visitorFunc);
    llvm::BasicBlock *innerLoopIncBB = llvm::BasicBlock::Create(context, "inner_loop_inc", visitorFunc);
    llvm::BasicBlock *loopNextBB = llvm::BasicBlock::Create(context, "loop_next", visitorFunc);
    llvm::BasicBlock *exitBB = llvm::BasicBlock::Create(context, "exit", visitorFunc);
    
    visitorBuilder.CreateBr(loopCondBB);
    
    // Loop condition: while (currentEntry != null)
    visitorBuilder.SetInsertPoint(loopCondBB);
    llvm::PHINode *entryPhi = visitorBuilder.CreatePHI(stackEntryPtrTy, 2, "entry_phi");
    entryPhi->addIncoming(currentEntry, entryBB);
    
    llvm::Value *isNonNull = visitorBuilder.CreateICmpNE(
        entryPhi, llvm::ConstantPointerNull::get(stackEntryPtrTy), "is_non_null");
    visitorBuilder.CreateCondBr(isNonNull, loopBodyBB, exitBB);
    
    // Loop body: process current stack entry
    visitorBuilder.SetInsertPoint(loopBodyBB);
    
    // Get frame map from current entry
    llvm::Value *frameMapPtr = visitorBuilder.CreateStructGEP(
        stack_entry_type, entryPhi, 1, "framemap_ptr");
    llvm::Value *frameMap = visitorBuilder.CreateLoad(frameMapPtrTy, frameMapPtr, "framemap");
    
    // Get number of roots from frame map
    llvm::Value *numRootsPtr = visitorBuilder.CreateStructGEP(
        frame_map_type, frameMap, 0, "num_roots_ptr");
    llvm::Value *numRoots = visitorBuilder.CreateLoad(i64Ty, numRootsPtr, "num_roots");
    
    // Get roots array pointer
    llvm::Value *rootsPtr = visitorBuilder.CreateStructGEP(
        stack_entry_type, entryPhi, 2, "roots_ptr");
    
    // Inner loop initialization
    llvm::Value *zeroIdx = llvm::ConstantInt::get(i64Ty, 0);
    visitorBuilder.CreateBr(innerLoopCondBB);
    
    // Inner loop condition: for (i = 0; i < numRoots; i++)
    visitorBuilder.SetInsertPoint(innerLoopCondBB);
    llvm::PHINode *idxPhi = visitorBuilder.CreatePHI(i64Ty, 2, "idx_phi");
    idxPhi->addIncoming(zeroIdx, loopBodyBB);
    
    llvm::Value *isLessThan = visitorBuilder.CreateICmpSLT(idxPhi, numRoots, "is_less_than");
    visitorBuilder.CreateCondBr(isLessThan, innerLoopBodyBB, loopNextBB);
    
    // Inner loop body: check and process each root
    visitorBuilder.SetInsertPoint(innerLoopBodyBB);
    
    // Get root pointer at index i
    llvm::Value *rootElementPtr = visitorBuilder.CreateGEP(
        i8PtrTy, rootsPtr, idxPhi, "root_element_ptr");
    llvm::Value *rootPtr = visitorBuilder.CreateLoad(i8PtrTy, rootElementPtr, "root_ptr");
    
    // Check if root is non-null
    llvm::Value *rootIsNonNull = visitorBuilder.CreateICmpNE(
        rootPtr, llvm::ConstantPointerNull::get(i8PtrTy), "root_is_non_null");
    
    llvm::BasicBlock *processRootBB = llvm::BasicBlock::Create(context, "process_root", visitorFunc);
    llvm::BasicBlock *skipRootBB = llvm::BasicBlock::Create(context, "skip_root", visitorFunc);
    
    visitorBuilder.CreateCondBr(rootIsNonNull, processRootBB, skipRootBB);
    
    // Process root block
    visitorBuilder.SetInsertPoint(processRootBB);
    
    // Get reference count (4 bytes before the data pointer)
    llvm::Value *refcntOffset = llvm::ConstantInt::get(i64Ty, -static_cast<int64_t>(REFCNT_SIZE));
    llvm::Value *refcntPtr = visitorBuilder.CreateGEP(
        llvm::Type::getInt8Ty(context), rootPtr, refcntOffset, "refcnt_ptr");
    
    llvm::PointerType *i32PtrTy = llvm::PointerType::get(context, 0);
    llvm::Value *refcntI32Ptr = visitorBuilder.CreateBitCast(refcntPtr, i32PtrTy, "refcnt_i32_ptr");
    llvm::Value *refCount = visitorBuilder.CreateLoad(i32Ty, refcntI32Ptr, "ref_count");
    
    // Check if reference count is zero
    llvm::Value *refCountIsZero = visitorBuilder.CreateICmpEQ(
        refCount, llvm::ConstantInt::get(i32Ty, 0), "ref_count_is_zero");
    
    llvm::BasicBlock *freeRootBB = llvm::BasicBlock::Create(context, "free_root", visitorFunc);
    
    visitorBuilder.CreateCondBr(refCountIsZero, freeRootBB, skipRootBB);
    
    // Free root block
    visitorBuilder.SetInsertPoint(freeRootBB);
    
    // Call free on the original allocation (including ref count)
    llvm::Function *freeFunc = module.getFunction("free");
    if (!freeFunc) {
      llvm::FunctionType *freeType = llvm::FunctionType::get(
          llvm::Type::getVoidTy(context), {llvm::PointerType::get(context, 0)}, false);
      freeFunc = llvm::Function::Create(freeType, llvm::Function::ExternalLinkage, 
                                       "free", &module);
    }
    visitorBuilder.CreateCall(freeFunc, {refcntPtr});
    
    // Clear the root pointer in the stack entry
    visitorBuilder.CreateStore(llvm::ConstantPointerNull::get(i8PtrTy), rootElementPtr);
    
    visitorBuilder.CreateBr(skipRootBB);
    
    // Skip root block - continue to next iteration
    visitorBuilder.SetInsertPoint(skipRootBB);
    visitorBuilder.CreateBr(innerLoopIncBB);
    
    // Inner loop increment
    visitorBuilder.SetInsertPoint(innerLoopIncBB);
    llvm::Value *nextIdx = visitorBuilder.CreateAdd(
        idxPhi, llvm::ConstantInt::get(i64Ty, 1), "next_idx");
    idxPhi->addIncoming(nextIdx, innerLoopIncBB);
    visitorBuilder.CreateBr(innerLoopCondBB);
    
    // Move to next stack entry
    visitorBuilder.SetInsertPoint(loopNextBB);
    llvm::Value *nextEntryPtr = visitorBuilder.CreateStructGEP(
        stack_entry_type, entryPhi, 0, "next_entry_ptr");
    llvm::Value *nextEntry = visitorBuilder.CreateLoad(stackEntryPtrTy, nextEntryPtr, "next_entry");
    entryPhi->addIncoming(nextEntry, loopNextBB);
    visitorBuilder.CreateBr(loopCondBB);
    
    // Exit block
    visitorBuilder.SetInsertPoint(exitBB);
    visitorBuilder.CreateRetVoid();
  }
}
} // namespace sammine_lang::AST
