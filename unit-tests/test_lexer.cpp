//
// Created by jjasmine on 3/8/24.
//

//! \file test_lexer.cpp
//! \brief The unit-test file for all things related to a lexer.
#include "lex/Lexer.h"
#include "lex/Token.h"
#include "util/Utilities.h"
#include <catch2/catch_test_macros.hpp>

//! Simple test cases for a Lexer, test for an identifier followed by a number
//! (both of length 1)
TEST_CASE("hello (lex) world", "[Lexer]") {
  sammine_lang::Lexer lex("a 2");

  SECTION("Test token type") {
    REQUIRE(lex.peek().get()->tok_type == sammine_lang::TokID);
    lex.consume();

    REQUIRE(lex.peek().get()->tok_type == sammine_lang::TokNum);
    lex.consume();
  }

  SECTION("Test lexeme") {
    REQUIRE(lex.peek().get()->lexeme == "a");
    lex.consume();

    REQUIRE(lex.peek().get()->lexeme == "2");
    lex.consume();
  }
}

TEST_CASE("Basic operators", "[Lexer]") {

  SECTION("+ related tokens") {
    sammine_lang::Lexer lex("++ += +");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokAddIncr);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokAddAssign);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokADD);
  }

  SECTION("- related tokens") {
    sammine_lang::Lexer lex("-- -= -");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokSubDecr);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokSubAssign);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokSUB);
  }

  SECTION("* related tokens") {
    sammine_lang::Lexer lex("** *= *");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokEXP);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokMulAssign);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokMUL);
  }

  SECTION("/ related tokens") {
    sammine_lang::Lexer lex("/= /_ /^ /");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokDivAssign);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokFloorDiv);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokCeilDiv);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokDIV);
  }

  SECTION("% related tokens") {
    sammine_lang::Lexer lex("%");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokMOD);
  }

  SECTION("& related tokens") {
    sammine_lang::Lexer lex("&& &");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokAND);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokAndLogical);
  }

  SECTION("| related tokens") {
    sammine_lang::Lexer lex("|| |");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokOR);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokORLogical);
  }

  SECTION("^ related tokens") {
    sammine_lang::Lexer lex("^ ");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokXOR);
  }

  SECTION("<< >> related tokens") {
    sammine_lang::Lexer lex("<< >> ");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokSHL);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokSHR);
  }

  SECTION("Basic comparision tokens") {
    sammine_lang::Lexer lex("== < > = ! <= >=");

    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokEQUAL);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokLESS);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokGREATER);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokASSIGN);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokNOT);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokLessEqual);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokGreaterEqual);
  }
}
TEST_CASE("Identifiers and keywords", "[Lexer]") {
  SECTION(" Keywords ") {
    sammine_lang::Lexer lex("let if else main fn");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokLet);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokIf);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokElse);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokID);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokFunc);
  }
}

TEST_CASE("Basic utility tokens", "[Lexer]") {
  SECTION("Parenthesis and curly tokens") {
    sammine_lang::Lexer lex("( ) { }");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokLeftParen);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokRightParen);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokLeftCurly);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokRightCurly);
  }

  SECTION(" Commas and Colons and pointing") {
    sammine_lang::Lexer lex(", . : :: ");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokComma);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokDot);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokColon);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokDoubleColon);
  }

  SECTION(" Comments") {
    sammine_lang::Lexer lex("# ");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokEOF);
  }

  SECTION("EOF") {
    sammine_lang::Lexer lex("");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokEOF);
  }
}

TEST_CASE("Complex combination", "[Lexer]") {
  SECTION("Parenthesis and curly tokens") {
    sammine_lang::Lexer lex("({}{)");
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokLeftParen);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokLeftCurly);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokRightCurly);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokLeftCurly);
    REQUIRE(lex.consume().get()->tok_type == sammine_lang::TokRightParen);
  }

  SECTION("Identifier complex test") {
    sammine_lang::Lexer lex("x2x2 xya2 func2 fn");

    auto token = lex.consume().get();
    REQUIRE(token->tok_type == sammine_lang::TokID);
    REQUIRE(token->lexeme == "x2x2");

    token = lex.consume().get();
    REQUIRE(token->tok_type == sammine_lang::TokID);
    REQUIRE(token->lexeme == "xya2");

    token = lex.consume().get();
    REQUIRE(token->tok_type == sammine_lang::TokID);
    REQUIRE(token->lexeme == "func2");

    token = lex.consume().get();
    REQUIRE(token->tok_type == sammine_lang::TokFunc);
  }

  SECTION("Identifer and operator") {
    sammine_lang::Lexer lex("x+=2.3");
    auto token = lex.consume().get();
    REQUIRE(token->tok_type == sammine_lang::TokID);
    REQUIRE(token->lexeme == "x");

    token = lex.consume().get();
    REQUIRE(token->tok_type == sammine_lang::TokAddAssign);

    token = lex.consume().get();
    REQUIRE(token->tok_type == sammine_lang::TokNum);
    REQUIRE(token->lexeme == "2.3");
  }

  SECTION("Function calls") {
    sammine_lang::Lexer lex("fn f(x:f64) -> f64 { }");

    auto tokStream = lex.getTokenStream();

    REQUIRE_FALSE(tokStream->hasErrors());
  }
}
