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
    llvm::PointerType *i32PtrTy = llvm::PointerType::get(i32Ty, 0);
    llvm::Value *refcntPtr =
        Builder.CreateBitCast(rawPtr, i32PtrTy, "refcnt_ptr");

    // Store refcnt = 1
    Builder.CreateStore(llvm::ConstantInt::get(i32Ty, 1), refcntPtr);

    // Return data pointer (i8*)
    Builder.CreateRet(rawPtr);
  }
}

void RefCounter::decrease_refcnt(llvm::Value *) {}
void RefCounter::increase_refcnt(llvm::Value *) {}
void RefCounter::declare_refcnt_visitor(bool is_main_file,
                                        llvm::StructType *stack_entry_type,
                                        llvm::StructType *frame_map_type) {

  // TODO: Declare the refcnt_visitor
  //
  //
  // TODO: Defines the refcnt_visitor function that takes a struct
  //
  //
  // TODO: get the num root
  //
  // TODO: construct a simple for loop
  //    - Get the ref cnt inside each root
  //    - if it is 0, free the root
}
} // namespace sammine_lang::AST
