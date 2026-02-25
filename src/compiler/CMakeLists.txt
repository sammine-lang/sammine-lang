
add_library(compiler OBJECT ${CMAKE_CURRENT_LIST_DIR}/Compiler.cpp)
target_include_directories(compiler PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(compiler PRIVATE project_options fmt LLVMSupport ${llvm_libs} ${llvm_targets}
  MLIRIR MLIRSupport
)
