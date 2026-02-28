
add_library(compiler OBJECT
  ${CMAKE_CURRENT_LIST_DIR}/Compiler.cpp
  ${CMAKE_CURRENT_LIST_DIR}/CompilerDB.cpp
)

add_library(CompilerDB OBJECT ${CMAKE_CURRENT_LIST_DIR}/CompilerDB.cpp)
target_link_libraries(CompilerDB PRIVATE project_options)
target_include_directories(compiler PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(compiler PRIVATE project_options fmt LLVMSupport ${llvm_libs} ${llvm_targets}
  MLIRIR MLIRSupport
)
