#pragma once

#include "ast/AstBase.h"
#include "util/Utilities.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Location.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

//! \file SourceLocator.h
//! \brief Maps an AST node's byte offsets to an MLIR file:line:col location.
//! Shared by the module's codegen and the MIR lowering, so both locate code
//! alike.

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
    auto [startLine, startCol] = lineAndColumn(lines, srcLoc.source_start);
    auto [endLine, endCol] = lineAndColumn(lines, srcLoc.source_end);
    auto &file =
        srcLoc.source_info ? srcLoc.source_info->file_name : fileName_;
    if (srcLoc.source_info)
      importedFiles_.try_emplace(file, srcLoc.source_info);
    // The whole span, so a diagnostic about the op underlines what the AST
    // node covers. Lines are 1-based, columns 0-based.
    return mlir::FileLineColRange::get(builder.getStringAttr(file), startLine,
                                       startCol, endLine, endCol);
  }

  /// The source span `loc` stands for, as loc() made it; nullopt for a
  /// location it didn't make.
  std::optional<sammine_util::Location> location(mlir::Location loc) const {
    if (auto name = mlir::dyn_cast<mlir::NameLoc>(loc))
      return location(name.getChildLoc());
    auto range = mlir::dyn_cast<mlir::FileLineColRange>(loc);
    if (!range)
      return std::nullopt;
    auto file = range.getFilename().getValue();
    std::shared_ptr<sammine_util::SourceInfo> sourceInfo;
    if (file != fileName_) {
      auto it = importedFiles_.find(file.str());
      if (it == importedFiles_.end())
        return std::nullopt;
      sourceInfo = it->second;
    }
    sammine_util::Location result;
    result.source_info = sourceInfo;
    auto &lines = sourceInfo ? linesOf(result) : mainLines_;
    auto offset = [&](unsigned line, unsigned col) {
      return lines[line - 1].first + static_cast<int64_t>(col);
    };
    if (range.getStartLine() == 0 || range.getEndLine() > lines.size())
      return std::nullopt;
    result.source_start = offset(range.getStartLine(), range.getStartColumn());
    result.source_end = offset(range.getEndLine(), range.getEndColumn());
    return result;
  }

private:
  using Lines = sammine_util::Reporter::DiagnosticData;

  /// The 1-based line and 0-based column of byte `offset`.
  static std::pair<unsigned, unsigned> lineAndColumn(const Lines &lines,
                                                     int64_t offset) {
    // The line holding offset: the last whose start is not after it.
    auto cmp = [](const auto &a, const auto &b) { return a.first < b.first; };
    auto it = std::ranges::upper_bound(
        lines, std::make_pair(offset, std::string_view("")), cmp);
    int64_t lineIdx = std::max(int64_t(it - lines.begin()), int64_t(1)) - 1;
    int64_t col = offset - lines[static_cast<size_t>(lineIdx)].first;
    return {static_cast<unsigned>(lineIdx + 1), static_cast<unsigned>(col)};
  }

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
  /// Each imported file loc() located something in, by its name, so
  /// location() can find its lines again.
  mutable std::unordered_map<std::string,
                             std::shared_ptr<sammine_util::SourceInfo>>
      importedFiles_;
};

} // namespace sammine_lang
