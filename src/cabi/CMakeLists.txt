# The C calling convention: how C moves structs and variadic arguments. Both
# the AST codegen and MIR's lowering to LLVM call C, so it is a library of its
# own, apart from either.
add_library(SammineCABI STATIC ${CMAKE_CURRENT_LIST_DIR}/CABI.cpp)
target_include_directories(SammineCABI PUBLIC ${INCLUDE_PROJECT_DIR})
target_include_directories(SammineCABI SYSTEM PUBLIC
  ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
)
target_link_libraries(SammineCABI PRIVATE project_options)
target_link_libraries(SammineCABI PUBLIC MLIRIR MLIRLLVMDialect MLIRDataLayoutInterfaces)
