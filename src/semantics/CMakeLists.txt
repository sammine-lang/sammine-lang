
add_project_library(NameResolver)
add_project_library(ScopeGeneratorVisitor)
add_project_library(GeneralSemanticsVisitor)

add_library(semantics)
target_link_libraries(semantics NameResolver ScopeGeneratorVisitor
                      GeneralSemanticsVisitor)
