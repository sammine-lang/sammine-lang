# Concrete Syntax Tree (CST) Infrastructure

## Overview

Lossless CST built alongside the AST using a Roslyn-style red-green tree architecture. Every byte of source is represented, enabling exact reconstruction (`root.text() == original_source`). The AST remains the primary path for all downstream passes; the CST is a parallel structure for future LSP use.

## Architecture: Three-Layer Syntax Tree

```
Layer 1: Green Tree    — immutable, position-free, internable/cacheable
Layer 2: Red Tree      — lazy wrappers with parent pointers + absolute offsets
Layer 3: Typed AST     — (future) zero-cost structs wrapping SyntaxNode
```

## Files

| File | Description |
|------|-------------|
| `include/cst/SyntaxKind.h` | Unified enum: tokens [0,1024), nodes [1024,2048) |
| `src/cst/SyntaxKind.cpp` | `syntax_kind_name()`, `from_token_type()` |
| `include/cst/GreenNode.h` | `GreenToken`, `GreenNode`, `GreenElement`, `GreenInterner` |
| `src/cst/GreenNode.cpp` | Arena allocation + token dedup |
| `include/cst/TreeBuilder.h` | Marker-based builder with checkpoint/rollback |
| `src/cst/TreeBuilder.cpp` | Builder implementation |
| `include/cst/SyntaxNode.h` | Red tree wrapper (`SyntaxNode`) + `SyntaxTree` |
| `src/cst/SyntaxNode.cpp` | Offset computation, `text()`, `node_at_offset()` |
| `src/cst/CMakeLists.txt` | `add_library(cst OBJECT ...)` |
| `include/compiler/CompilerDB.h` | Query-based cache with revision tracking |
| `src/compiler/CompilerDB.cpp` | Lex + build CST, cached by revision |

## SyntaxKind Enum

Unifies `TokenType` and `NodeKind` into a single `SyntaxKind : uint16_t`:
- Tokens: `[0, 1024)` — maps 1:1 from `TokenType` via `from_token_type()`
- Nodes: `[1024, 2048)` — `SourceFile`, `FuncDef`, `BinaryExpr`, etc.
- Trivia tokens: `TokWhitespace`, `TokNewline`, `TokSingleComment`
- Helpers: `is_token()`, `is_node()`, `is_trivia()`, `syntax_kind_name()`

## Green Tree (Immutable, Position-Free)

- **`GreenToken`** — leaf: `SyntaxKind` + text (flexible array member in arena). Created via `GreenInterner::token()` with structural dedup (same kind+text → same pointer).
- **`GreenNode`** — interior: `SyntaxKind` + cached `text_len_` + children array (flexible array member). Created via `GreenInterner::node()`.
- **`GreenElement`** — tagged pointer (low bit: 0=token, 1=node). Provides `kind()`, `text_len()`, `is_token()`, `is_node()`.
- **`GreenInterner`** — arena allocator (`std::pmr::monotonic_buffer_resource`) + `unordered_map` token dedup cache. NOT movable (pmr limitation).

Key invariant: `GreenNode::text_len()` = sum of children's `text_len()`. No absolute positions stored — only widths.

## TreeBuilder (Parser's Construction API)

Marker-based builder:
```cpp
builder.start_node(SyntaxKind::FuncDef);
builder.token(SyntaxKind::TokLet, "let");
builder.token(SyntaxKind::TokWhitespace, " ");
builder.finish_node();
```

Speculative parsing support:
- `checkpoint()` → `CheckpointId` — saves element/node stack positions
- `rollback_to(CheckpointId)` — discards accumulated elements since checkpoint
- `abandon(Marker)` — discards started node wrapper, keeps its children

## Red Tree (SyntaxNode — Lazy, With Positions)

- **`SyntaxNode`** — 24 bytes: `GreenNode*` + `SyntaxNode* parent` + `uint32_t offset`. Created on-the-fly from green tree.
- **`SyntaxChild`** — `GreenElement` + absolute offset.
- Methods: `kind()`, `text_len()`, `offset()`, `end_offset()`, `children()`, `child_node()`, `child_token_text()`, `text()` (lossless), `node_at_offset()`, `dump()`.
- **`SyntaxTree`** — owns `unique_ptr<GreenInterner>` + root `GreenNode*`. Provides `root()` returning `SyntaxNode`.

## Dual-Destination Lexer Emission

The lexer emits tokens to two destinations:
1. **`TokenStream`** — filtered, no trivia tokens. Parser uses this unchanged.
2. **`raw_tokens_`** — `std::vector<shared_ptr<Token>>` of ALL tokens including `TokWhitespace`, `TokNewline`, `TokSingleComment`.

The `emit()` helper in `Lexer.h` pushes to both. Trivia-only tokens (`TokWhitespace`, `TokNewline`, `TokSingleComment`) are pushed to `raw_tokens_` only via direct `raw_tokens_.push_back()`.

Access raw tokens: `lexer.getRawTokens()`.

## CST Construction (Current: Flat)

The CST is currently built as a **flat** tree: a single `SourceFile` node with all tokens as direct children. Built from the raw token vector after parsing completes.

Special handling:
- **TokEOF** — skipped (lexeme "end of file" doesn't match source)
- **TokStr / TokChar** — use raw source text via `input.substr(source_start, source_end - source_start)` instead of processed lexeme (which has quotes stripped and escapes resolved)

Verification: `root.text_len() == source.size()` and `root.text() == source` (lossless round-trip). All 307+ e2e tests pass this check.

## CompilerDB (Query-Based Cache)

```cpp
CompilerDB db;
db.set_source("fn main() -> i32 { 42 }");
const auto &tree = db.parse();   // computes and caches
const auto &tree2 = db.parse();  // returns cached (same revision)
db.set_source("fn main() -> i32 { 0 }");  // bumps revision
const auto &tree3 = db.parse();  // re-parses (new revision)
```

- `set_source()` bumps `revision_`
- `parse()` only re-lexes/rebuilds CST when `revision_ != cst_revision_`
- `is_cached()` checks `cached_cst_.has_value() && cst_revision_ == revision_`
- Foundation for LSP `textDocument/didChange` support

## CLI Flag

`--cst-ir` — dumps CST structure and verifies lossless round-trip. Added to `compiler_option_enum` as `CST_IR`.

## Build System

- `src/cst/CMakeLists.txt` defines `cst` OBJECT library
- `src/compiler/CMakeLists.txt` defines separate `CompilerDB` OBJECT library (for unit test linking without pulling in full compiler + MLIR deps)
- `unit-tests/CMakeLists.txt` links `cst` and `CompilerDB`
- Unit tests in `unit-tests/test_cst.cpp`

## Gotchas

- `GreenInterner` is NOT movable (`std::pmr::monotonic_buffer_resource` is not movable) — always use `std::make_unique<GreenInterner>()` and pass by reference
- `is_cached()` must check `cached_cst_.has_value()` in addition to revision match, since both `cst_revision_` and `revision_` start at 0
- TokAND lexeme was `"&"` instead of `"&&"` in the operator rule table — a pre-existing bug exposed by CST lossless verification
- The CST and AST intentionally diverge where the parser desugars (pipe operator, `**` double deref, implicit returns)

## Future Work

- **Phase 2 full**: Thread TreeBuilder through parser for structured (non-flat) CST with proper node nesting
- **Phase 4**: CST-to-AST bridge — walk CST to produce AST, making CST the single source of truth
- **Phase 5**: Typed CST wrappers replacing AST classes, with downstream passes migrating one-by-one
