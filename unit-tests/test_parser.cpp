//
// Created by jjasmine on 3/16/24.
//

//! \file test_parser.cpp
//! \brief The unit-test file for all things related to a parser.
#include "ast/Ast.h"
#include "lex/Token.h"
#include "parser/Parser.h"
#include <catch2/catch_test_macros.hpp>

using namespace sammine_lang;

TEST_CASE("Empty program parsing", "[Parser]") {
  auto lexer = sammine_lang::Lexer("");
  auto tokStreamPtr = lexer.getTokenStream();
  auto pg = sammine_lang::Parser(tokStreamPtr);

  auto programAST = pg.Parse();

  REQUIRE_FALSE(pg.has_errors());
}

TEST_CASE("Variable definition parsing", "[Parser]") {

  SECTION("Variable definition with number") {
    auto lexer = sammine_lang::Lexer("let b : blablabla = 0;");
    auto tokStreamPtr = lexer.getTokenStream();
    auto pg = sammine_lang::Parser(tokStreamPtr);

    auto programAST = pg.ParseVarDef();

    REQUIRE_FALSE(pg.has_errors());

    auto varDef =
        static_cast<sammine_lang::AST::VarDefAST *>(programAST.first.get());
    REQUIRE_TRUE(varDef->TypedVar->name == "b");
    REQUIRE_TRUE(varDef->TypedVar->type_lexeme == "blablabla");
  }

  SECTION("Variable definition with binary expression") {
    auto lexer = sammine_lang::Lexer("let b : blablabla = 1+2*3;");
    auto tokStreamPtr = lexer.getTokenStream();
    auto pg = sammine_lang::Parser(tokStreamPtr);

    auto programAST = pg.ParseVarDef();

    REQUIRE_FALSE(pg.has_errors());

    auto varDef =
        static_cast<sammine_lang::AST::VarDefAST *>(programAST.first.get());
    REQUIRE_TRUE(varDef->TypedVar->name == "b");
    REQUIRE_TRUE(varDef->TypedVar->type_lexeme == "blablabla");
  }

  SECTION("Variable definition with call expression") {
    auto lexer = sammine_lang::Lexer("let b : blablabla = hi();");
    auto tokStreamPtr = lexer.getTokenStream();
    auto pg = sammine_lang::Parser(tokStreamPtr);

    auto programAST = pg.ParseVarDef();

    REQUIRE_FALSE(pg.has_errors());

    auto varDef =
        static_cast<sammine_lang::AST::VarDefAST *>(programAST.first.get());
    REQUIRE_TRUE(varDef->TypedVar->name == "b");
    REQUIRE_TRUE(varDef->TypedVar->type_lexeme == "blablabla");

    auto expr =
        static_cast<sammine_lang::AST::CallExprAST *>(varDef->Expression.get());
    REQUIRE_TRUE(expr->functionName == "hi");
    REQUIRE_TRUE(expr->arguments.size() == 0);
  }

  SECTION("Variable definition with string literal expression") {
    auto lexer = sammine_lang::Lexer("let b : blablabla = \"how aboutthis\" ");
    auto tokStreamPtr = lexer.getTokenStream();
    auto pg = sammine_lang::Parser(tokStreamPtr);

    auto programAST = pg.ParseVarDef();

    auto varDef =
        static_cast<sammine_lang::AST::VarDefAST *>(programAST.first.get());
    REQUIRE_TRUE(varDef->TypedVar->name == "b");
    REQUIRE_TRUE(varDef->TypedVar->type_lexeme == "blablabla");
  }
}

TEST_CASE("Function declaration parsing", "[Parser]") {
  // func f(x:f64) -> f64 {
  //  return 0;
  //}

  SECTION("Single argument") {
    auto lex = Lexer("fn f(x:f64) -> f64 {\n \n }");
    REQUIRE_TRUE(lex.getTokenStream()->hasErrors() == false);
    auto pg = Parser(lex.getTokenStream());

    auto programAST = pg.Parse();
    REQUIRE_FALSE(pg.has_errors());
    REQUIRE_TRUE(programAST->DefinitionVec.size() == 1);

    auto func_def =
        static_cast<AST::FuncDefAST *>(programAST->DefinitionVec.front().get());

    // Check if downcast is valid.
    REQUIRE_TRUE(func_def != nullptr);

    REQUIRE_TRUE(func_def->Prototype->returnType == "f64");
    REQUIRE_TRUE(func_def->Prototype->functionName == "f");
    REQUIRE_TRUE(func_def->Prototype->parameterVectors.size() == 1);
  }

  SECTION("Multiple argument") {
    auto lex = Lexer("fn f(x:f64, y : hi, z : hoe) {\n  \n }");
    REQUIRE_TRUE(lex.getTokenStream()->hasErrors() == false);
    auto pg = Parser(lex.getTokenStream());

    auto programAST = pg.Parse();
    REQUIRE_FALSE(pg.has_errors());

    REQUIRE_TRUE(programAST->DefinitionVec.size() == 1);

    auto func_def =
        static_cast<AST::FuncDefAST *>(programAST->DefinitionVec.front().get());

    // Check if downcast is valid.
    REQUIRE_TRUE(func_def != nullptr);

    REQUIRE_TRUE(func_def->Prototype->functionName == "f");
    REQUIRE_TRUE(func_def->Prototype->parameterVectors.size() == 3);
  }

  SECTION("None return") {
    auto lex = Lexer("fn f() {\n  \n }");

    // sammine_lang::Lexer lex("fn f(x:f64, y : hi, z : hoe) -> f64 {\n return
    // 2; \n }");

    auto pg = Parser(lex.getTokenStream());
    auto programAST = pg.Parse();

    REQUIRE_FALSE(pg.has_errors());
  }
}

TEST_CASE("FAILED TO PARSE", "[Parser]") {
  auto lex = Lexer("a a a a a");
  REQUIRE_FALSE(lex.has_errors());
  auto pg = Parser(lex.getTokenStream());
  auto programAST = pg.Parse();
  REQUIRE_TRUE(pg.has_errors());
}
