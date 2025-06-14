
add_project_library(SammineJIT)
add_project_library(TypeConverter)
add_project_library(Garbage)

add_library(CodegenVisitor OBJECT ${CMAKE_CURRENT_LIST_DIR}/CodegenVisitor.cpp ${CMAKE_CURRENT_LIST_DIR}/CodegenUtils.cpp ${CMAKE_CURRENT_LIST_DIR}/FunctionCodegen.cpp )
target_link_libraries(CodegenVisitor PRIVATE project_options)

# Final linkage for folder
add_library(_codegen)
target_include_directories(_codegen PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS})
target_link_libraries(_codegen SammineJIT CodegenVisitor TypeConverter Garbage LLVMSupport ${llvm_libs} ${llvm_targets})
