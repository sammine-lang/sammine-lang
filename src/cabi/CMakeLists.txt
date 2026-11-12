# Calling conventions: how C moves structs and variadic arguments, and how a
# kernel's wrapper takes arrays. Both the AST codegen and MIR's lowering to
# LLVM call C and kernels, so it is a library of its own, apart from either.
add_library(SammineCABI STATIC ${CMAKE_CURRENT_LIST_DIR}/CABI.cpp
  ${CMAKE_CURRENT_LIST_DIR}/KernelABI.cpp)
target_include_directories(SammineCABI PUBLIC ${INCLUDE_PROJECT_DIR})
target_include_directories(SammineCABI SYSTEM PUBLIC
  ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
)
target_link_libraries(SammineCABI PRIVATE project_options)
target_link_libraries(SammineCABI PUBLIC
  MLIRIR MLIRLLVMDialect MLIRArithDialect MLIRDataLayoutInterfaces
)
