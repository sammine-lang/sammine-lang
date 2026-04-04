//
// Created by Jasmine Tang on 3/27/24.
//

#pragma once
#include "compiler/Options.h"
//! \file Compiler.h
//! \brief Define the Compiler staging
namespace sammine_lang {


class CompilerRunner {
public:
  static int run(const Options &options);
};

} // namespace sammine_lang
