#pragma once

#include "ast/AstBase.h"
#include "util/Utilities.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Location.h"

#include <algorithm>
#include <string>
#include <unordered_map>

//! \file SourceLocator.h
//! \brief Maps an AST node's byte offsets to an MLIR file:line:col location.
//! Shared by the AST codegen and the MIR lowering, so both locate code alike.

namespace sammine_lang {

class SourceLocator {
public:
  /// `sourceText` must outlive the locator: its lines are kept as views. It
  /// is the main file's; code from an imported module carries its own file in
  /// its Location's source_info.
  SourceLocator(std::string fileName, std::string_view sourceText)
      : fileName_(std::move(fileName)),
        mainLines_(sammine_util::Reporter::get_diagnostic_data(sourceText)) {}

  mlir::Location loc(mlir::Builder &builder, const AST::AstBase *ast) const {
    if (!ast)
      return builder.getUnknownLoc();
    auto srcLoc = ast->get_location();
    auto &lines = linesOf(srcLoc);
    if ((srcLoc.source_start <= 0 && srcLoc.source_end <= 0) || lines.empty())
      return builder.getUnknownLoc();

    // The line holding source_start: the last whose start is not after it.
    auto cmp = [](const auto &a, const auto &b) { return a.first < b.first; };
    auto it = std::ranges::upper_bound(
        lines, std::make_pair(srcLoc.source_start, std::string_view("")), cmp);
    int64_t lineIdx = std::max(int64_t(it - lines.begin()), int64_t(1)) - 1;
    int64_t col = srcLoc.source_start - lines[static_cast<size_t>(lineIdx)].first;

    // FileLineColLoc uses 1-based line and 0-based column
    auto &file =
        srcLoc.source_info ? srcLoc.source_info->file_name : fileName_;
    return mlir::FileLineColLoc::get(builder.getContext(), file,
                                     static_cast<unsigned>(lineIdx + 1),
                                     static_cast<unsigned>(col));
  }

private:
  using Lines = sammine_util::Reporter::DiagnosticData;

  /// The lines of the file `srcLoc` is in. An imported file's are split once
  /// and kept: codegen asks for a location for nearly every op.
  const Lines &linesOf(const sammine_util::Location &srcLoc) const {
    if (!srcLoc.source_info)
      return mainLines_;
    auto [it, inserted] = importedLines_.try_emplace(srcLoc.source_info.get());
    if (inserted)
      it->second = sammine_util::Reporter::get_diagnostic_data(
          srcLoc.source_info->source_text);
    return it->second;
  }

  std::string fileName_;
  Lines mainLines_;
  /// Keyed by the SourceInfo the lexer gave the file's tokens; the AST holds
  /// those alive for as long as codegen runs.
  mutable std::unordered_map<const sammine_util::SourceInfo *, Lines>
      importedLines_;
};

} // namespace sammine_lang
