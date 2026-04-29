
add_library(compiler OBJECT ${CMAKE_CURRENT_LIST_DIR}/Compiler.cpp ${CMAKE_CURRENT_LIST_DIR}/resolver.cpp ${CMAKE_CURRENT_LIST_DIR}/library_emitter.cpp)
add_dependencies(compiler SammineOpsIncGen)
target_include_directories(compiler PRIVATE
  ${INCLUDE_PROJECT_DIR} ${CMAKE_BINARY_DIR}/src/codegen
)
target_include_directories(compiler SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/externals/stablehlo
  ${CMAKE_BINARY_DIR}/externals/stablehlo
)
target_link_libraries(compiler PRIVATE project_options fmt LLVMSupport ${llvm_libs} ${llvm_targets}
  MLIRIR MLIRSupport CLI11::CLI11
  StablehloOps
)
add_library(options OBJECT ${CMAKE_CURRENT_LIST_DIR}/options.cpp)
target_include_directories(options PRIVATE ${INCLUDE_PROJECT_DIR})
target_link_libraries(options PRIVATE project_options fmt CLI11::CLI11)
