add_executable(SammineCheck SammineCheck.cpp)

# No dependencies - standalone tool
target_compile_features(SammineCheck PRIVATE cxx_std_17)

# Install to build directory for easy access during testing
set_target_properties(SammineCheck PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
