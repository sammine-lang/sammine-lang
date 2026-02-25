
add_project_library(SammineJIT)
add_project_library(TypeConverter)

add_library(CodegenVisitor OBJECT ${CMAKE_CURRENT_LIST_DIR}/CodegenVisitor.cpp ${CMAKE_CURRENT_LIST_DIR}/CodegenUtils.cpp ${CMAKE_CURRENT_LIST_DIR}/FunctionCodegen.cpp )
target_link_libraries(CodegenVisitor PRIVATE project_options)

add_library(MLIRBackend OBJECT
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGen.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenFunction.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenExpr.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRLowering.cpp
)
target_include_directories(MLIRBackend PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(MLIRBackend PRIVATE
  project_options
  MLIRIR MLIRParser MLIRSupport MLIRPass
  MLIRArithDialect MLIRFuncDialect MLIRSCFDialect MLIRMemRefDialect
  MLIRArithToLLVM MLIRFuncToLLVM
  MLIRSCFToControlFlow MLIRControlFlowToLLVM MLIRMemRefToLLVM
  MLIRReconcileUnrealizedCasts
  MLIRTargetLLVMIRExport MLIRLLVMToLLVMIRTranslation MLIRBuiltinToLLVMIRTranslation
  MLIRLLVMDialect
)

# Final linkage for folder
add_library(_codegen)
target_include_directories(_codegen PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(_codegen SammineJIT CodegenVisitor TypeConverter MLIRBackend LLVMSupport ${llvm_libs} ${llvm_targets}
  MLIRIR MLIRParser MLIRSupport MLIRPass
  MLIRArithDialect MLIRFuncDialect MLIRSCFDialect MLIRMemRefDialect
  MLIRArithToLLVM MLIRFuncToLLVM
  MLIRSCFToControlFlow MLIRControlFlowToLLVM MLIRMemRefToLLVM
  MLIRReconcileUnrealizedCasts
  MLIRTargetLLVMIRExport MLIRLLVMToLLVMIRTranslation MLIRBuiltinToLLVMIRTranslation
  MLIRLLVMDialect
)
