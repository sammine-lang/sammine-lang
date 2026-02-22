//
// Created by Jasmine Tang on 4/3/24.
//

#include "lex/Lexer.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Unrecognized tokens", "[Invalid Lexer]") {
  sammine_lang::Lexer lex("$#$@");
  auto tokStream = lex.getTokenStream();
  SECTION("Test token type") {
    tokStream->exhaust_until(sammine_lang::TokEOF);
    REQUIRE(tokStream->hasErrors());
  }


}