cmake_minimum_required(VERSION 3.22)
project(sammine_lang )
enable_language(CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)
endif()
add_subdirectory(externals/)

# LLVM-STUFF
# find_package(ZSTD REQUIRED)

# Check to see if were on a apple
if(APPLE AND NOT DEFINED LLVM_DIR)
  execute_process(
    COMMAND brew --prefix llvm
    OUTPUT_VARIABLE BREW_PREFIX_LLVM
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  # Mark the following LLVM_DIR to be where it's set on the Mac
  set(LLVM_DIR "${BREW_PREFIX_LLVM}/lib/cmake/llvm" CACHE PATH "Path to LLVMConfig.cmake")
endif()

find_package(LLVM REQUIRED CONFIG)
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS_LIST})
llvm_map_components_to_libnames(llvm_libs native orcjit core support irreader)

foreach(target ${LLVM_TARGETS_TO_BUILD})
  list(APPEND llvm_targets "LLVM${target}CodeGen")
endforeach()

# MLIR
if(APPLE AND NOT DEFINED MLIR_DIR)
  if(NOT BREW_PREFIX_LLVM)
    execute_process(
      COMMAND brew --prefix llvm
      OUTPUT_VARIABLE BREW_PREFIX_LLVM
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  endif()
  set(MLIR_DIR "${BREW_PREFIX_LLVM}/lib/cmake/mlir" CACHE PATH "Path to MLIRConfig.cmake")
endif()
find_package(MLIR REQUIRED CONFIG)
include_directories(SYSTEM ${MLIR_INCLUDE_DIRS})




option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

# Find ccache
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CUDA_COMPILER_LAUNCHER "${CCACHE_PROGRAM}") # CMake 3.9+
endif()

# PROJECT_INCLUDE_DIR
SET(INCLUDE_PROJECT_DIR "${CMAKE_SOURCE_DIR}/include/")

add_subdirectory(src)
add_subdirectory(tools)

# --- Standard library ---
# Compile stdlib modules using the freshly-built sammine binary. Each module
# produces a .so in build/lib/sammine/. The .mn source is also copied so that
# `import` can parse it directly (no .mni files).
set(STDLIB_DIR ${CMAKE_BINARY_DIR}/lib/sammine)
file(MAKE_DIRECTORY ${STDLIB_DIR})

set(STDLIB_COMPILED std str)
set(STDLIB_COPIED definitions.mn vec.mn)

foreach(mod ${STDLIB_COMPILED})
  add_custom_command(
    OUTPUT ${STDLIB_DIR}/${mod}.so
    COMMAND $<TARGET_FILE:sammine> -f ${CMAKE_SOURCE_DIR}/stdlib/${mod}.mn -O ${STDLIB_DIR}
    DEPENDS sammine ${CMAKE_SOURCE_DIR}/stdlib/${mod}.mn
    COMMENT "Compiling stdlib/${mod}.mn → ${mod}.so"
  )
  # Copy .mn source for import resolution
  add_custom_command(
    OUTPUT ${STDLIB_DIR}/${mod}.mn
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/stdlib/${mod}.mn ${STDLIB_DIR}/${mod}.mn
    DEPENDS ${CMAKE_SOURCE_DIR}/stdlib/${mod}.mn
    COMMENT "Copying stdlib/${mod}.mn"
  )
  list(APPEND STDLIB_OUTPUTS ${STDLIB_DIR}/${mod}.so ${STDLIB_DIR}/${mod}.mn)
endforeach()

foreach(file ${STDLIB_COPIED})
  add_custom_command(
    OUTPUT ${STDLIB_DIR}/${file}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/stdlib/${file} ${STDLIB_DIR}/${file}
    DEPENDS ${CMAKE_SOURCE_DIR}/stdlib/${file}
    COMMENT "Copying stdlib/${file}"
  )
  list(APPEND STDLIB_OUTPUTS ${STDLIB_DIR}/${file})
endforeach()

# --- io module: sammine wrapper + C runtime (auto-detected) ---
# Compile io_runtime.c → io_runtime.o
add_custom_command(
  OUTPUT ${STDLIB_DIR}/io_runtime.o
  COMMAND ${CMAKE_C_COMPILER} -c ${CMAKE_SOURCE_DIR}/stdlib/io_runtime.c
          -o ${STDLIB_DIR}/io_runtime.o
  DEPENDS ${CMAKE_SOURCE_DIR}/stdlib/io_runtime.c
  COMMENT "Compiling stdlib/io_runtime.c"
)

# Compile io.mn → io.so (auto-detects io_runtime.o in -O dir)
add_custom_command(
  OUTPUT ${STDLIB_DIR}/io.so
  COMMAND $<TARGET_FILE:sammine> -f ${CMAKE_SOURCE_DIR}/stdlib/io.mn
          -O ${STDLIB_DIR}
  DEPENDS sammine ${CMAKE_SOURCE_DIR}/stdlib/io.mn ${STDLIB_DIR}/io_runtime.o
  COMMENT "Compiling stdlib/io.mn → io.so (with C runtime)"
)

# Copy io.mn source for import resolution
add_custom_command(
  OUTPUT ${STDLIB_DIR}/io.mn
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/stdlib/io.mn ${STDLIB_DIR}/io.mn
  DEPENDS ${CMAKE_SOURCE_DIR}/stdlib/io.mn
  COMMENT "Copying stdlib/io.mn"
)

list(APPEND STDLIB_OUTPUTS ${STDLIB_DIR}/io.so ${STDLIB_DIR}/io.mn)

add_custom_target(stdlib ALL DEPENDS ${STDLIB_OUTPUTS})

option(SAMMINE_TEST "Enable the test in sammine-lang repository(disabled by default)" OFF)
include(CTest)
if (SAMMINE_TEST)
  enable_testing()
  add_subdirectory(unit-tests)
  add_subdirectory(e2e-tests)
endif()




