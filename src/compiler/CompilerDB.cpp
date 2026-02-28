#include "compiler/CompilerDB.h"
#include "cst/CSTBridge.h"
#include "cst/SyntaxKind.h"
#include "lex/Lexer.h"
#include "parser/Parser.h"

namespace sammine_lang {

void CompilerDB::set_source(std::string text) {
  source_text_ = std::move(text);
  revision_++;
}

const cst::SyntaxTree &CompilerDB::parse() {
  if (cst_revision_ == revision_ && cached_cst_.has_value())
    return *cached_cst_;

  // Lex the source
  Lexer lexer(source_text_);
  auto tok_stream = lexer.getTokenStream();

  // Exhaust the lexer to produce all tokens before parsing
  tok_stream->exhaust_until(TokEOF);
  tok_stream->reset_cursor();

  // Cache the raw tokens
  cached_raw_tokens_ = lexer.getRawTokens();

  // Set up CST infrastructure with CSTBridge
  auto interner = std::make_unique<cst::GreenInterner>();
  cst::TreeBuilder builder(*interner);
  cst::CSTBridge bridge(builder, cached_raw_tokens_, source_text_);

  // Start root SourceFile directly on builder (not bridge) so leading
  // trivia goes INSIDE the SourceFile node
  builder.start_node(cst::SyntaxKind::SourceFile);

  // Install on_consume_ callback to route tokens to CSTBridge
  tok_stream->set_on_consume(
      [&bridge](const std::shared_ptr<Token> &tok) {
        bridge.on_token_consumed(tok);
      });

  // Parse into AST (CSTBridge receives tokens via callback)
  Parser parser(tok_stream);
  parser.set_cst_bridge(&bridge);
  (void)parser.Parse();

  // Remove callback before bridge goes out of scope
  tok_stream->set_on_consume(nullptr);

  // Finish CST: emit trailing trivia, close SourceFile
  bridge.finish_remaining();
  bridge.finish_node();

  auto *green_root = bridge.finish();
  cached_cst_.emplace(std::move(interner), green_root);
  cst_revision_ = revision_;

  return *cached_cst_;
}

} // namespace sammine_lang
