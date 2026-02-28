#pragma once

#include "cst/GreenNode.h"
#include "cst/SyntaxNode.h"
#include "cst/TreeBuilder.h"
#include "lex/Token.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sammine_lang {

/// Query-based compilation database. Caches parse results (CST) keyed by
/// source revision. Foundation for LSP `textDocument/didChange` support.
///
/// Usage:
///   CompilerDB db;
///   db.set_source("fn main() -> i32 { 42 }");
///   const auto &tree = db.parse();  // computes and caches
///   const auto &tree2 = db.parse(); // returns cached (same revision)
///   db.set_source("fn main() -> i32 { 0 }"); // bumps revision
///   const auto &tree3 = db.parse(); // re-parses (new revision)
class CompilerDB {
  std::string source_text_;
  uint64_t revision_ = 0;

  // Cached CST
  std::optional<cst::SyntaxTree> cached_cst_;
  uint64_t cst_revision_ = 0;

  // Cached raw tokens (produced during parsing)
  std::vector<std::shared_ptr<Token>> cached_raw_tokens_;

public:
  CompilerDB() = default;

  /// Set new source text. Bumps the revision counter.
  void set_source(std::string text);

  /// Get the current source text.
  const std::string &source_text() const { return source_text_; }

  /// Get the current revision number.
  uint64_t revision() const { return revision_; }

  /// Lazily parse and return the cached CST. Only re-parses when
  /// the revision has changed since the last parse.
  const cst::SyntaxTree &parse();

  /// Check if the CST cache is up-to-date.
  bool is_cached() const { return cached_cst_.has_value() && cst_revision_ == revision_; }
};

} // namespace sammine_lang
