cmake_minimum_required(VERSION 3.22)
project(sammine_lang )
enable_language(CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SAMMINE_SANITIZE "Enable ASan + UBSan (debug builds)" OFF)
option(SAMMINE_TRACY "Enable Tracy profiler instrumentation" OFF)
add_subdirectory(externals/)

# LLVM-STUFF
find_package(ZLIB REQUIRED)
find_package(zstd REQUIRED)

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
# Nix clang wrapper already defines _LIBCPP_HARDENING_MODE; drop LLVM's copy to avoid -Wmacro-redefined
list(FILTER LLVM_DEFINITIONS_LIST EXCLUDE REGEX "_LIBCPP_HARDENING_MODE")
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS_LIST})
llvm_map_components_to_libnames(llvm_libs native orcjit core support irreader)

set(llvm_targets "")
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
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}" "${LLVM_CMAKE_DIR}")
include(TableGen)
include(AddMLIR)

# Derive LLVM lib directory for runtime library linking (e.g. libmlir_cuda_runtime)
get_filename_component(MLIR_LLVM_LIB_DIR "${MLIR_DIR}/../.." ABSOLUTE)
add_compile_definitions(MLIR_LLVM_LIB_DIR="${MLIR_LLVM_LIB_DIR}")

# StableHLO (built embedded, reuses the MLIR found above)
set(STABLEHLO_BUILD_EMBEDDED ON CACHE BOOL "" FORCE)
add_subdirectory(externals/stablehlo EXCLUDE_FROM_ALL)




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
# Each compiled module produces .so, .a, and a copied .mn source in
# build/lib/sammine/.  Modules with a C runtime (e.g. io) list their
# .c file in STDLIB_C_RUNTIME_<mod>.
set(STDLIB_DIR ${CMAKE_BINARY_DIR}/lib/sammine)
file(MAKE_DIRECTORY ${STDLIB_DIR})

set(STDLIB_COMPILED std str io)
set(STDLIB_COPIED definitions.mn vec.mn)
set(STDLIB_C_RUNTIME_io io_runtime.c)

# Helper: compile a C runtime file to a PIC object in STDLIB_DIR.
function(stdlib_compile_c_runtime src out_var)
  get_filename_component(stem ${src} NAME_WE)
  set(obj ${STDLIB_DIR}/${stem}.o)
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CMAKE_C_COMPILER} -fPIC -c ${CMAKE_SOURCE_DIR}/stdlib/${src} -o ${obj}
    DEPENDS ${CMAKE_SOURCE_DIR}/stdlib/${src}
    COMMENT "Compiling stdlib/${src}"
  )
  set(${out_var} ${obj} PARENT_SCOPE)
endfunction()

# Helper: compile a .mn module to .so + .a and copy the source.
function(stdlib_add_module mod extra_deps)
  set(src ${CMAKE_SOURCE_DIR}/stdlib/${mod}.mn)
  add_custom_command(
    OUTPUT ${STDLIB_DIR}/${mod}.so
    COMMAND $<TARGET_FILE:sammine> -f ${src} -O ${STDLIB_DIR}
    DEPENDS sammine ${src} ${extra_deps}
    COMMENT "Compiling stdlib/${mod}.mn -> ${mod}.so"
  )
  add_custom_command(
    OUTPUT ${STDLIB_DIR}/${mod}.a
    COMMAND $<TARGET_FILE:sammine> -f ${src} -O ${STDLIB_DIR} --lib=static
    DEPENDS sammine ${src} ${extra_deps} ${STDLIB_DIR}/${mod}.so
    COMMENT "Compiling stdlib/${mod}.mn -> ${mod}.a"
  )
  add_custom_command(
    OUTPUT ${STDLIB_DIR}/${mod}.mn
    COMMAND ${CMAKE_COMMAND} -E copy ${src} ${STDLIB_DIR}/${mod}.mn
    DEPENDS ${src}
    COMMENT "Copying stdlib/${mod}.mn"
  )
  set(STDLIB_OUTPUTS ${STDLIB_OUTPUTS}
      ${STDLIB_DIR}/${mod}.so ${STDLIB_DIR}/${mod}.a ${STDLIB_DIR}/${mod}.mn
      PARENT_SCOPE)
endfunction()

foreach(mod ${STDLIB_COMPILED})
  set(_extra_deps "")
  if(STDLIB_C_RUNTIME_${mod})
    stdlib_compile_c_runtime(${STDLIB_C_RUNTIME_${mod}} _obj)
    list(APPEND _extra_deps ${_obj})
  endif()
  stdlib_add_module(${mod} "${_extra_deps}")
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

add_custom_target(stdlib ALL DEPENDS ${STDLIB_OUTPUTS})

option(SAMMINE_TEST "Enable the test in sammine-lang repository(disabled by default)" OFF)
if (SAMMINE_TEST)
  include(CTest)
  enable_testing()
  add_subdirectory(unit-tests)
  add_subdirectory(e2e-tests)
endif()




