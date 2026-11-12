#pragma once

#include "ast/AstBase.h"
#include "util/Utilities.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Location.h"

#include <algorithm>
#include <string>

//! \file SourceLocator.h
//! \brief Maps an AST node's byte offsets to an MLIR file:line:col location.
//! Shared by the AST codegen and the MIR lowering, so both locate code alike.

namespace sammine_lang {

class SourceLocator {
public:
  /// `sourceText` must outlive the locator: its lines are kept as views.
  SourceLocator(std::string fileName, std::string_view sourceText)
      : fileName_(std::move(fileName)),
        lines_(sammine_util::Reporter::get_diagnostic_data(sourceText)) {}

  mlir::Location loc(mlir::Builder &builder, const AST::AstBase *ast) const {
    if (!ast)
      return builder.getUnknownLoc();
    auto srcLoc = ast->get_location();
    if ((srcLoc.source_start <= 0 && srcLoc.source_end <= 0) || lines_.empty())
      return builder.getUnknownLoc();

    // The line holding source_start: the last whose start is not after it.
    auto cmp = [](const auto &a, const auto &b) { return a.first < b.first; };
    auto it = std::ranges::upper_bound(
        lines_, std::make_pair(srcLoc.source_start, std::string_view("")), cmp);
    int64_t lineIdx = std::max(int64_t(it - lines_.begin()), int64_t(1)) - 1;
    int64_t col = srcLoc.source_start - lines_[static_cast<size_t>(lineIdx)].first;

    // FileLineColLoc uses 1-based line and 0-based column
    return mlir::FileLineColLoc::get(builder.getContext(), fileName_,
                                     static_cast<unsigned>(lineIdx + 1),
                                     static_cast<unsigned>(col));
  }

private:
  std::string fileName_;
  sammine_util::Reporter::DiagnosticData lines_;
};

} // namespace sammine_lang
