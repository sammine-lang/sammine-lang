include(FetchContent)

# Save main project build type
# set(MAIN_PROJECT_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
#
# # Force dependencies to be built in Release
# set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Set build type for dependencies" FORCE)
# message("Setting external dependencies build type to: ${CMAKE_BUILD_TYPE}")

# Fetch and build dependencies
FetchContent_Declare(
  cpptrace
  GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
  GIT_TAG        v1.0.0
  SYSTEM
)
FetchContent_MakeAvailable(cpptrace)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG        11.2.0
  SYSTEM
)
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
    argparse
    GIT_REPOSITORY https://github.com/p-ranav/argparse.git
    SYSTEM
)
FetchContent_MakeAvailable(argparse)

# Restore main project build type
# set(CMAKE_BUILD_TYPE "${MAIN_PROJECT_BUILD_TYPE}" CACHE STRING "Restore main project build type" FORCE)
# message("Restoring main project build type to: ${CMAKE_BUILD_TYPE}")
