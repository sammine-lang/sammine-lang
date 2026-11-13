# The sammine dialect: its TableGen definitions, its C++, and the passes that
# work on it. Kept apart from codegen, which builds IR from the AST.

# The .td files include each other as dialect/*.td.
include_directories(${INCLUDE_PROJECT_DIR})
set(LLVM_TARGET_DEFINITIONS ${INCLUDE_PROJECT_DIR}/dialect/SammineOps.td)
mlir_tablegen(SammineOps.h.inc -gen-op-decls)
mlir_tablegen(SammineOps.cpp.inc -gen-op-defs)
mlir_tablegen(SammineDialect.h.inc -gen-dialect-decls)
mlir_tablegen(SammineDialect.cpp.inc -gen-dialect-defs)
set(LLVM_TARGET_DEFINITIONS ${INCLUDE_PROJECT_DIR}/dialect/SammineTypes.td)
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

# The dialect's types and ops. Its generated headers are found through it.
# They are MLIR's code, not ours, so like MLIR's own headers they are system
# headers: our warnings don't apply to them.
add_library(SammineDialect STATIC ${CMAKE_CURRENT_LIST_DIR}/SammineDialect.cpp)
add_dependencies(SammineDialect SammineOpsIncGen)
target_include_directories(SammineDialect PUBLIC ${INCLUDE_PROJECT_DIR})
target_include_directories(SammineDialect SYSTEM PUBLIC
  ${CMAKE_CURRENT_BINARY_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
)
target_link_libraries(SammineDialect PRIVATE project_options)
target_link_libraries(SammineDialect PUBLIC
  MLIRIR MLIRSupport MLIRFunctionInterfaces MLIRLLVMDialect
)

# Passes on the dialect: the ownership analysis and check, MIR to the LLVM
# dialect, and the GPU transfer ops to the gpu dialect.
add_library(SammineDialectTransforms STATIC
  ${CMAKE_CURRENT_LIST_DIR}/OwnershipPaths.cpp
  ${CMAKE_CURRENT_LIST_DIR}/OwnershipAnalysis.cpp
  ${CMAKE_CURRENT_LIST_DIR}/OwnershipCheck.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ViewCheck.cpp
  ${CMAKE_CURRENT_LIST_DIR}/LambdaSets.cpp
  ${CMAKE_CURRENT_LIST_DIR}/SpecializeLambdaSets.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ElaborateDrops.cpp
  ${CMAKE_CURRENT_LIST_DIR}/MIRToLLVM.cpp
  ${CMAKE_CURRENT_LIST_DIR}/LowerSammineOps.cpp
)
target_link_libraries(SammineDialectTransforms PRIVATE project_options)
target_link_libraries(SammineDialectTransforms PUBLIC
  SammineDialect SammineCABI MLIRArithDialect MLIRFuncDialect MLIRFuncTransforms MLIRControlFlowTransforms MLIRLLVMDialect MLIRDataLayoutInterfaces
  MLIRGPUDialect MLIRMemRefDialect MLIRTransformUtils MLIRPass MLIRAnalysis
)
