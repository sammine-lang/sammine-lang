add_executable(sammine-opt sammine-opt.cpp)
target_link_libraries(sammine-opt PRIVATE
  project_options SammineDialect SammineDialectTransforms
  MLIROptLib MLIRParser MLIRFuncDialect MLIRArithDialect MLIRControlFlowDialect
)
set_target_properties(sammine-opt PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
