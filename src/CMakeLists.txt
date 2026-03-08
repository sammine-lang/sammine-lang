
add_library(project_options INTERFACE)
target_include_directories(project_options INTERFACE
  ${INCLUDE_PROJECT_DIR}
)
target_link_libraries(project_options INTERFACE
  LLVMSupport
  ${llvm_libs}
  fmt::fmt
)

# Strict warnings — scoped to our code only (not third-party deps)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(project_options INTERFACE
        -Wall -Wextra -Werror -Wpedantic
        -Wno-unused-parameter
        -Wshadow
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wcast-align
        -Wold-style-cast
        -Wconversion
        -Wsign-conversion
    )
endif()

if(SAMMINE_SANITIZE)
    target_compile_options(project_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(project_options INTERFACE
        -fsanitize=address,undefined)
endif()

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
target_include_directories(sammine PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
target_link_libraries(sammine project_options compiler cpptrace::cpptrace util _ast Lexer Parser
     semantics typecheck Types Monomorphizer _codegen  fmt argparse)

set_target_properties(sammine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
