
add_project_library(SammineJIT)

# --- Sammine dialect: TableGen, and a library of its own ---
# The .td files include each other as codegen/*.td.
include_directories(${INCLUDE_PROJECT_DIR})
set(LLVM_TARGET_DEFINITIONS ${INCLUDE_PROJECT_DIR}/codegen/SammineOps.td)
mlir_tablegen(SammineOps.h.inc -gen-op-decls)
mlir_tablegen(SammineOps.cpp.inc -gen-op-defs)
mlir_tablegen(SammineDialect.h.inc -gen-dialect-decls)
mlir_tablegen(SammineDialect.cpp.inc -gen-dialect-defs)
set(LLVM_TARGET_DEFINITIONS ${INCLUDE_PROJECT_DIR}/codegen/SammineTypes.td)
mlir_tablegen(SammineTypes.h.inc -gen-typedef-decls -typedefs-dialect=sammine)
mlir_tablegen(SammineTypes.cpp.inc -gen-typedef-defs -typedefs-dialect=sammine)
mlir_tablegen(SammineEnums.h.inc -gen-enum-decls)
mlir_tablegen(SammineEnums.cpp.inc -gen-enum-defs)
add_custom_target(SammineOpsIncGen DEPENDS
  ${CMAKE_CURRENT_BINARY_DIR}/SammineOps.h.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineOps.cpp.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineDialect.h.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineDialect.cpp.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineTypes.h.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineTypes.cpp.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineEnums.h.inc
  ${CMAKE_CURRENT_BINARY_DIR}/SammineEnums.cpp.inc
)

# The dialect alone, so tools like sammine-opt need not link the backend.
# Its generated headers are MLIR's code, not ours, so like MLIR's own headers
# they are system headers: our warnings don't apply to them.
add_library(SammineDialect STATIC ${CMAKE_CURRENT_LIST_DIR}/SammineDialect.cpp)
add_dependencies(SammineDialect SammineOpsIncGen)
target_include_directories(SammineDialect PUBLIC ${INCLUDE_PROJECT_DIR})
target_include_directories(SammineDialect SYSTEM PUBLIC
  ${CMAKE_CURRENT_BINARY_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
)
target_link_libraries(SammineDialect PRIVATE project_options)
target_link_libraries(SammineDialect PUBLIC MLIRIR MLIRSupport)

# MIR's passes, also for tools.
add_library(SammineMIRTransforms STATIC ${CMAKE_CURRENT_LIST_DIR}/MIRToLLVM.cpp)
target_link_libraries(SammineMIRTransforms PRIVATE project_options)
target_link_libraries(SammineMIRTransforms PUBLIC
  SammineDialect MLIRFuncDialect MLIRFuncTransforms MLIRLLVMDialect
  MLIRTransformUtils MLIRPass
)

add_library(MLIRBackend OBJECT
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGen.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenFunction.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MIRLowering.cpp
  ${CMAKE_CURRENT_LIST_DIR}/CABI.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenExpr.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenBinaryOps.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRGenKernel.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MLIRLowering.cpp
  ${CMAKE_CURRENT_LIST_DIR}/LinalgReduceToGpu.cpp
  ${CMAKE_CURRENT_LIST_DIR}/LowerSammineOps.cpp
)
add_dependencies(MLIRBackend SammineOpsIncGen)
target_include_directories(MLIRBackend PRIVATE ${INCLUDE_PROJECT_DIR})
target_include_directories(MLIRBackend SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
)
target_link_libraries(MLIRBackend PRIVATE
  project_options SammineDialect SammineMIRTransforms
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
target_include_directories(_codegen PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(_codegen SammineJIT MLIRBackend SammineDialect SammineMIRTransforms LLVMSupport ${llvm_libs} ${llvm_targets})
