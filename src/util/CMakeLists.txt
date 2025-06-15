add_library(util OBJECT ${CMAKE_CURRENT_LIST_DIR}/Utilities.cpp)
target_link_libraries(util PRIVATE project_options fmt::fmt cpptrace::cpptrace)
