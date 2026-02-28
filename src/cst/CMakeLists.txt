add_library(cst OBJECT
  SyntaxKind.cpp
  GreenNode.cpp
  TreeBuilder.cpp
  SyntaxNode.cpp
  CSTBridge.cpp
)
target_link_libraries(cst PRIVATE project_options)
