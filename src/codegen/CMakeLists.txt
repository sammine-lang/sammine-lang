
add_project_library(SammineJIT)

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
  MLIRArithDialect MLIRFuncDialect MLIRSCFDialect
  MLIRArithToLLVM MLIRFuncToLLVM
  MLIRSCFToControlFlow MLIRControlFlowToLLVM
  MLIRReconcileUnrealizedCasts
  MLIRTargetLLVMIRExport MLIRLLVMToLLVMIRTranslation MLIRBuiltinToLLVMIRTranslation
  MLIRLLVMDialect
  # Phase 7a: kernel linalg codegen
  MLIRLinalgDialect MLIRTensorDialect MLIRBufferizationDialect
  MLIRAffineDialect MLIRMemRefDialect
  MLIRLinalgTransforms MLIRBufferizationTransforms MLIRBufferizationPipelines
  MLIRMemRefTransforms
  MLIRMemRefToLLVM MLIRAffineToStandard
  MLIRArithTransforms MLIRControlFlowTransforms MLIRLinalgTransforms
  MLIRSCFTransforms MLIRTensorTransforms
  MLIRTransforms
)

# Final linkage for folder
add_library(_codegen)
target_include_directories(_codegen PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(_codegen SammineJIT MLIRBackend LLVMSupport ${llvm_libs} ${llvm_targets}
  MLIRIR MLIRParser MLIRSupport MLIRPass
  MLIRArithDialect MLIRFuncDialect MLIRSCFDialect
  MLIRArithToLLVM MLIRFuncToLLVM
  MLIRSCFToControlFlow MLIRControlFlowToLLVM
  MLIRReconcileUnrealizedCasts
  MLIRTargetLLVMIRExport MLIRLLVMToLLVMIRTranslation MLIRBuiltinToLLVMIRTranslation
  MLIRLLVMDialect
  # Phase 7a: kernel linalg codegen
  MLIRLinalgDialect MLIRTensorDialect MLIRBufferizationDialect
  MLIRAffineDialect MLIRMemRefDialect
  MLIRLinalgTransforms MLIRBufferizationTransforms MLIRBufferizationPipelines
  MLIRMemRefTransforms
  MLIRMemRefToLLVM MLIRAffineToStandard
  MLIRArithTransforms MLIRControlFlowTransforms MLIRLinalgTransforms
  MLIRSCFTransforms MLIRTensorTransforms
  MLIRTransforms
)
