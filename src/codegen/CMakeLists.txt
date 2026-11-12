
add_project_library(SammineJIT)

add_library(MLIRBackend OBJECT
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGen.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenFunction.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MIRLowering.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MIRTypes.cpp
  ${CMAKE_CURRENT_LIST_DIR}/TypeLayout.cpp
  ${CMAKE_CURRENT_LIST_DIR}/RangeIteration.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenBinaryOps.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenKernel.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRLowering.cpp
  ${CMAKE_CURRENT_LIST_DIR}/LinalgReduceToGpu.cpp
)
add_dependencies(MLIRBackend SammineOpsIncGen)
target_include_directories(MLIRBackend PRIVATE ${INCLUDE_PROJECT_DIR})
target_include_directories(MLIRBackend SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
)
target_link_libraries(MLIRBackend PRIVATE
  project_options SammineDialect SammineDialectTransforms SammineCABI
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
  # Phase 7c: GPU codegen
  MLIRGPUDialect MLIRGPUTransforms
  MLIRNVVMDialect
  MLIRGPUToNVVMTransforms
  MLIRGPUToLLVMIRTranslation
  MLIRSCFToGPU
  MLIRNVVMToLLVM
  MLIRUBToLLVM
  MLIRIndexToLLVM
  MLIRMathToLLVM
  MLIRComplexToLLVM
  MLIRVectorToLLVM
  MLIRNVVMTarget
  MLIRRegisterAllExtensions
  MLIRRegisterAllDialects
)

# Final linkage for folder
add_library(_codegen)
target_include_directories(_codegen PRIVATE ${INCLUDE_PROJECT_DIR})
target_include_directories(_codegen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(_codegen SammineJIT MLIRBackend SammineDialect SammineDialectTransforms SammineCABI LLVMSupport ${llvm_libs} ${llvm_targets})
