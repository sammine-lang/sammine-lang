
add_library(project_options INTERFACE)
target_include_directories(project_options INTERFACE 
  ${INCLUDE_PROJECT_DIR}
)
target_link_libraries(project_options INTERFACE 
  LLVMSupport
  ${llvm_libs}
  fmt::fmt
)

# Function to add a library with common settings
function(add_project_library name)
    add_library(${name} OBJECT ${CMAKE_CURRENT_LIST_DIR}/${name}.cpp)
    target_link_libraries(${name} PRIVATE project_options)
endfunction()




add_project_library(Parser)
add_subdirectory(lex)
add_subdirectory(ast)
add_subdirectory(codegen)
add_subdirectory(typecheck)
add_subdirectory(semantics)
add_subdirectory(util)


add_subdirectory(compiler)


add_executable(sammine ${CMAKE_CURRENT_LIST_DIR}/sammine.cpp)
target_include_directories(sammine PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS})

target_link_libraries(sammine compiler cpptrace::cpptrace util _ast Lexer Parser 
     semantics typecheck Types _codegen  fmt argparse)
