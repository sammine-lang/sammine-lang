#pragma once

#include "typecheck/Types.h"

#include <string>

//! \file RangeIteration.h
//! \brief What a `for i in a..b` loop calls and gets back, for the AST
//! codegen and MIR alike: Iterator<Range<T>, T>::next, which gives
//! Option<(Range<T>, T)>.

namespace sammine_lang {

struct RangeIteration {
  /// (Range<T>, T): the range after this step, and this step's element.
  Type step;
  /// Option<(Range<T>, T)>, Some (variant 0) of a step or None (variant 1).
  Type option;
  /// The symbol of Iterator<Range<T>, T>::next.
  std::string next;
};

/// The iteration over `range` (a Range<T>) of elements of `element` (T).
RangeIteration rangeIteration(const Type &range, const Type &element);

} // namespace sammine_lang
