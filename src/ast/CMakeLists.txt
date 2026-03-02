
add_project_library(Ast)
add_project_library(AstPrinterVisitor)
add_project_library(AstToString)
add_project_library(AstWalkers)


add_library(_ast)
target_include_directories(_ast PRIVATE ${INCLUDE_PROJECT_DIR} ${LLVM_INCLUDE_DIRS})
target_link_libraries(_ast Ast AstPrinterVisitor AstToString AstWalkers)
