add_executable(SammineCheck SammineCheck.cpp)

# No dependencies - standalone tool
target_compile_features(SammineCheck PRIVATE cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(SammineCheck PRIVATE
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

# Install to build directory for easy access during testing
set_target_properties(SammineCheck PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
