# Add a custom target for running ctest that depends on the lit tests
include(CTest)
# configure_file(lit.cfg.py .)
cmake_host_system_information(RESULT LIT_WORKERS QUERY NUMBER_OF_LOGICAL_CORES)
add_custom_target(e2e-tests
    COMMAND lit --succinct -j ${LIT_WORKERS} ${CMAKE_SOURCE_DIR}/e2e-tests
    DEPENDS tests sammine SammineCheck stdlib
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

