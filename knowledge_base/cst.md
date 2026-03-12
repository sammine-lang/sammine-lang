# CST Infrastructure

Lossless Concrete Syntax Tree built alongside AST. Roslyn-style red-green tree architecture. Every byte preserved (`root.text() == source`). AST is primary for all passes; CST is parallel for future LSP.

## Three Layers
1. **Green Tree** (immutable, position-free): `GreenToken` (leaf, flex array in arena) + `GreenNode` (interior, cached text_len) + `GreenElement` (tagged pointer). `GreenInterner` = arena allocator + token dedup.
2. **Red Tree** (lazy, with positions): `SyntaxNode` = 24 bytes (GreenNode* + parent + offset). Created on-the-fly. `SyntaxTree` owns interner + root.
3. **Typed AST** (future): zero-cost structs wrapping SyntaxNode.

## SyntaxKind
- Unifies TokenType and NodeKind into `uint16_t`: tokens [0,1024), nodes [1024,2048)
- Trivia: TokWhitespace, TokNewline, TokSingleComment

## Key Invariant
`GreenNode::text_len()` = sum of children's text_len. No absolute positions — only widths.

## Current State: Flat
- Single `SourceFile` node with all tokens as direct children (built from raw token vector post-parse)
- TokEOF skipped; TokStr/TokChar use raw source text (not processed lexeme)
- Verified via `root.text_len() == source.size()` on all 307+ e2e tests

## CompilerDB (Query-Based Cache)
- `set_source()` bumps revision; `parse()` only rebuilds when revision changed
- Foundation for LSP `textDocument/didChange`

## Dual Lexer Emission
- `TokenStream` (filtered, no trivia) for parser
- `raw_tokens_` (all tokens including trivia) for CST
- `emit()` pushes to both; trivia-only pushed to raw only

## Gotchas
- `GreenInterner` is NOT movable (pmr limitation) — always `make_unique`
- CST and AST diverge where parser desugars (pipe, `**` double deref, implicit returns)
