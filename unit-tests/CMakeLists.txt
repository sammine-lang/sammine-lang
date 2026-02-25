find_package(Catch2 3 REQUIRED)
include(Catch)

file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

set(lib_source Lexer Parser util _ast typecheck fmt::fmt)

add_executable(tests)
target_sources(tests PRIVATE ${test_sources})

target_include_directories(tests PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS})
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain ${llvm_libs} ${lib_source})

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

catch_discover_tests(tests  WORKING_DIRECTORY  ${CMAKE_CURRENT_LIST_DIR}
DISCOVERY_MODE PRE_TEST)

add_custom_target(unit-tests
  COMMAND ctest --test-dir ${CMAKE_SOURCE_DIR}/build --output-on-failure -j 0
  DEPENDS tests
)

