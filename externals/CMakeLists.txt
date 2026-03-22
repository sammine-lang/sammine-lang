include(FetchContent)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF CACHE BOOL "" FORCE)

# Force a target to be compiled with Release flags regardless of CMAKE_BUILD_TYPE.
# With single-config generators, FetchContent targets inherit the global build type.
# We override per-target by stripping debug flags and adding -O2 -DNDEBUG.
function(target_force_release target)
  if(NOT TARGET ${target})
    return()
  endif()
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  target_compile_options(${target} PRIVATE -O2 -g0)
  target_compile_definitions(${target} PRIVATE NDEBUG)
endfunction()

# --- Fetch and build dependencies ---

FetchContent_Declare(
  cpptrace
  GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
  GIT_TAG        v1.0.0
  SYSTEM
)
# Disable C++20 modules detection — Unix Makefiles doesn't support them
set(CPPTRACE_DISABLE_CXX_20_MODULES ON)
FetchContent_MakeAvailable(cpptrace)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG        11.2.0
  SYSTEM
)
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    SYSTEM
)
FetchContent_MakeAvailable(CLI11)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    SYSTEM
)
FetchContent_MakeAvailable(Catch2)

# Force all dependency targets to Release optimization.
# Last -O flag wins with GCC/Clang, so -O2 overrides -O0 from Debug mode.
foreach(_dep cpptrace-lib dwarf libzstd_static fmt Catch2 Catch2WithMain)
  target_force_release(${_dep})
endforeach()
