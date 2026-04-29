
add_project_library(SammineJIT)

# --- Sammine dialect tablegen ---
set(LLVM_TARGET_DEFINITIONS ${INCLUDE_PROJECT_DIR}/codegen/SammineOps.td)
mlir_tablegen(SammineOps.h.inc -gen-op-decls)
mlir_tablegen(SammineOps.cpp.inc -gen-op-defs)
mlir_tablegen(SammineDialect.h.inc -gen-dialect-decls)
mlir_tablegen(SammineDialect.cpp.inc -gen-dialect-defs)
add_custom_target(SammineOpsIncGen DEPENDS
  ${CMAKE_CURRENT_BINARY_DIR}/SammineOps.h.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineOps.cpp.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineDialect.h.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineDialect.cpp.inc
)

add_library(MLIRBackend OBJECT
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGen.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenFunction.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenExpr.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenBinaryOps.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenKernel.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRLowering.cpp
  ${CMAKE_CURRENT_LIST_DIR}/LinalgReduceToGpu.cpp
  ${CMAKE_CURRENT_LIST_DIR}/SammineDialect.cpp
  ${CMAKE_CURRENT_LIST_DIR}/LowerSammineOps.cpp
)
add_dependencies(MLIRBackend SammineOpsIncGen)
target_include_directories(MLIRBackend PRIVATE
  ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/externals/stablehlo
  ${CMAKE_BINARY_DIR}/externals/stablehlo
)
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
  # StableHLO
  StablehloOps
  StablehloLinalgTransforms
)

# Final linkage for folder
add_library(_codegen)
target_include_directories(_codegen PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(_codegen SammineJIT MLIRBackend LLVMSupport ${llvm_libs} ${llvm_targets})
