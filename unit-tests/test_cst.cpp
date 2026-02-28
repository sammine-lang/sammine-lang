#include <catch2/catch_test_macros.hpp>
#include "compiler/CompilerDB.h"
#include "cst/SyntaxKind.h"
#include "cst/GreenNode.h"
#include "cst/TreeBuilder.h"
#include "cst/SyntaxNode.h"

using namespace sammine_lang::cst;

// === SyntaxKind tests ===

TEST_CASE("SyntaxKind helpers", "[cst]") {
  SECTION("is_token for token kinds") {
    REQUIRE(is_token(SyntaxKind::TokADD));
    REQUIRE(is_token(SyntaxKind::TokEOF));
    REQUIRE(is_token(SyntaxKind::TokWhitespace));
    REQUIRE(is_token(SyntaxKind::TokNewline));
  }

  SECTION("is_node for node kinds") {
    REQUIRE(is_node(SyntaxKind::SourceFile));
    REQUIRE(is_node(SyntaxKind::FuncDef));
    REQUIRE(is_node(SyntaxKind::Block));
  }

  SECTION("is_token returns false for nodes") {
    REQUIRE_FALSE(is_token(SyntaxKind::SourceFile));
    REQUIRE_FALSE(is_token(SyntaxKind::FuncDef));
  }

  SECTION("is_node returns false for tokens") {
    REQUIRE_FALSE(is_node(SyntaxKind::TokADD));
    REQUIRE_FALSE(is_node(SyntaxKind::TokEOF));
  }

  SECTION("is_trivia") {
    REQUIRE(is_trivia(SyntaxKind::TokWhitespace));
    REQUIRE(is_trivia(SyntaxKind::TokNewline));
    REQUIRE(is_trivia(SyntaxKind::TokSingleComment));
    REQUIRE_FALSE(is_trivia(SyntaxKind::TokADD));
    REQUIRE_FALSE(is_trivia(SyntaxKind::TokID));
  }

  SECTION("syntax_kind_name returns names") {
    REQUIRE(syntax_kind_name(SyntaxKind::TokADD) == "TokADD");
    REQUIRE(syntax_kind_name(SyntaxKind::SourceFile) == "SourceFile");
    REQUIRE(syntax_kind_name(SyntaxKind::TokWhitespace) == "TokWhitespace");
  }

  SECTION("from_token_type conversion") {
    REQUIRE(from_token_type(sammine_lang::TokADD) == SyntaxKind::TokADD);
    REQUIRE(from_token_type(sammine_lang::TokEOF) == SyntaxKind::TokEOF);
    REQUIRE(from_token_type(sammine_lang::TokFunc) == SyntaxKind::TokFunc);
    REQUIRE(from_token_type(sammine_lang::TokID) == SyntaxKind::TokID);
  }
}

// === Green tree tests ===

TEST_CASE("GreenToken creation", "[cst]") {
  GreenInterner interner;

  auto *tok = interner.token(SyntaxKind::TokLet, "let");
  REQUIRE(tok->kind() == SyntaxKind::TokLet);
  REQUIRE(tok->text() == "let");
  REQUIRE(tok->text_len() == 3);
}

TEST_CASE("GreenToken interning dedup", "[cst]") {
  GreenInterner interner;

  auto *tok1 = interner.token(SyntaxKind::TokLet, "let");
  auto *tok2 = interner.token(SyntaxKind::TokLet, "let");
  REQUIRE(tok1 == tok2); // Same pointer — deduped

  auto *tok3 = interner.token(SyntaxKind::TokID, "let");
  REQUIRE(tok1 != tok3); // Different kind — not deduped

  auto *tok4 = interner.token(SyntaxKind::TokLet, "letx");
  REQUIRE(tok1 != tok4); // Different text — not deduped
}

TEST_CASE("GreenNode construction", "[cst]") {
  GreenInterner interner;

  auto *let_tok = interner.token(SyntaxKind::TokLet, "let");
  auto *ws = interner.token(SyntaxKind::TokWhitespace, " ");
  auto *id = interner.token(SyntaxKind::TokID, "x");

  std::vector<GreenElement> children;
  children.emplace_back(let_tok);
  children.emplace_back(ws);
  children.emplace_back(id);

  auto *node = interner.node(SyntaxKind::VarDef, children);
  REQUIRE(node->kind() == SyntaxKind::VarDef);
  REQUIRE(node->num_children() == 3);
  REQUIRE(node->text_len() == 5); // "let" + " " + "x" = 5
}

TEST_CASE("GreenElement tagged pointer", "[cst]") {
  GreenInterner interner;

  auto *tok = interner.token(SyntaxKind::TokADD, "+");
  GreenElement elem_tok(tok);
  REQUIRE(elem_tok.is_token());
  REQUIRE_FALSE(elem_tok.is_node());
  REQUIRE(elem_tok.kind() == SyntaxKind::TokADD);
  REQUIRE(elem_tok.text_len() == 1);

  std::vector<GreenElement> children;
  children.emplace_back(tok);
  auto *node = interner.node(SyntaxKind::BinaryExpr, children);
  GreenElement elem_node(node);
  REQUIRE(elem_node.is_node());
  REQUIRE_FALSE(elem_node.is_token());
  REQUIRE(elem_node.kind() == SyntaxKind::BinaryExpr);
}

// === TreeBuilder tests ===

TEST_CASE("TreeBuilder basic usage", "[cst]") {
  GreenInterner interner;
  TreeBuilder builder(interner);

  builder.start_node(SyntaxKind::SourceFile);
  builder.token(SyntaxKind::TokLet, "let");
  builder.token(SyntaxKind::TokWhitespace, " ");
  builder.token(SyntaxKind::TokID, "x");
  builder.finish_node();

  auto *root = builder.finish();
  REQUIRE(root->kind() == SyntaxKind::SourceFile);
  REQUIRE(root->num_children() == 3);
  REQUIRE(root->text_len() == 5);
}

TEST_CASE("TreeBuilder nested nodes", "[cst]") {
  GreenInterner interner;
  TreeBuilder builder(interner);

  builder.start_node(SyntaxKind::SourceFile);
  {
    builder.start_node(SyntaxKind::VarDef);
    builder.token(SyntaxKind::TokLet, "let");
    builder.token(SyntaxKind::TokWhitespace, " ");
    builder.token(SyntaxKind::TokID, "x");
    builder.finish_node();
  }
  builder.finish_node();

  auto *root = builder.finish();
  REQUIRE(root->kind() == SyntaxKind::SourceFile);
  REQUIRE(root->num_children() == 1);
  REQUIRE(root->child(0).is_node());
  REQUIRE(root->child(0).as_node()->kind() == SyntaxKind::VarDef);
  REQUIRE(root->child(0).as_node()->num_children() == 3);
}

TEST_CASE("TreeBuilder checkpoint and rollback", "[cst]") {
  GreenInterner interner;
  TreeBuilder builder(interner);

  builder.start_node(SyntaxKind::SourceFile);
  builder.token(SyntaxKind::TokLet, "let");

  auto cp = builder.checkpoint();

  // Speculatively add more tokens
  builder.start_node(SyntaxKind::VarDef);
  builder.token(SyntaxKind::TokWhitespace, " ");
  builder.token(SyntaxKind::TokID, "x");

  // Rollback — discard the speculative tokens and the started node
  builder.rollback_to(cp);

  // Add different content
  builder.token(SyntaxKind::TokWhitespace, " ");
  builder.token(SyntaxKind::TokID, "y");
  builder.finish_node();

  auto *root = builder.finish();
  REQUIRE(root->kind() == SyntaxKind::SourceFile);
  REQUIRE(root->num_children() == 3); // "let" " " "y"
  REQUIRE(root->text_len() == 5); // "let" + " " + "y"
}

TEST_CASE("TreeBuilder abandon", "[cst]") {
  GreenInterner interner;
  TreeBuilder builder(interner);

  builder.start_node(SyntaxKind::SourceFile);

  auto m = builder.start_node(SyntaxKind::VarDef);
  builder.token(SyntaxKind::TokLet, "let");
  builder.token(SyntaxKind::TokWhitespace, " ");
  // Abandon the VarDef wrapper — children become direct children of SourceFile
  builder.abandon(m);

  builder.token(SyntaxKind::TokID, "x");
  builder.finish_node();

  auto *root = builder.finish();
  REQUIRE(root->kind() == SyntaxKind::SourceFile);
  REQUIRE(root->num_children() == 3); // "let", " ", "x" as direct children
}

// === SyntaxNode tests ===

// Helper: create a SyntaxTree using heap-allocated interner
static SyntaxTree make_tree(std::unique_ptr<GreenInterner> interner,
                            const GreenNode *root) {
  return SyntaxTree(std::move(interner), root);
}

TEST_CASE("SyntaxNode offset computation", "[cst]") {
  auto interner = std::make_unique<GreenInterner>();
  TreeBuilder builder(*interner);

  // Build: SourceFile { VarDef { "let" " " "x" } "\n" VarDef { "let" " " "y" } }
  builder.start_node(SyntaxKind::SourceFile);
  {
    builder.start_node(SyntaxKind::VarDef);
    builder.token(SyntaxKind::TokLet, "let");
    builder.token(SyntaxKind::TokWhitespace, " ");
    builder.token(SyntaxKind::TokID, "x");
    builder.finish_node();
  }
  builder.token(SyntaxKind::TokNewline, "\n");
  {
    builder.start_node(SyntaxKind::VarDef);
    builder.token(SyntaxKind::TokLet, "let");
    builder.token(SyntaxKind::TokWhitespace, " ");
    builder.token(SyntaxKind::TokID, "y");
    builder.finish_node();
  }
  builder.finish_node();

  auto *green_root = builder.finish();
  auto tree = make_tree(std::move(interner), green_root);
  auto root = tree.root();

  REQUIRE(root.kind() == SyntaxKind::SourceFile);
  REQUIRE(root.offset() == 0);
  // "let x\nlet y" = 11 bytes total
  REQUIRE(root.text_len() == 11);

  auto children = root.children();
  REQUIRE(children.size() == 3);

  // First VarDef at offset 0, text_len = "let" + " " + "x" = 5
  REQUIRE(children[0].green.is_node());
  REQUIRE(children[0].offset == 0);
  REQUIRE(children[0].green.text_len() == 5);

  // Newline at offset 5
  REQUIRE(children[1].offset == 5);
  REQUIRE(children[1].green.text_len() == 1);

  // Second VarDef at offset 6, text_len = 5
  REQUIRE(children[2].offset == 6);
  REQUIRE(children[2].green.text_len() == 5);
}

TEST_CASE("SyntaxNode lossless text round-trip", "[cst]") {
  auto interner = std::make_unique<GreenInterner>();
  TreeBuilder builder(*interner);

  std::string source = "let x = 42;";

  builder.start_node(SyntaxKind::SourceFile);
  builder.token(SyntaxKind::TokLet, "let");
  builder.token(SyntaxKind::TokWhitespace, " ");
  builder.token(SyntaxKind::TokID, "x");
  builder.token(SyntaxKind::TokWhitespace, " ");
  builder.token(SyntaxKind::TokASSIGN, "=");
  builder.token(SyntaxKind::TokWhitespace, " ");
  builder.token(SyntaxKind::TokNum, "42");
  builder.token(SyntaxKind::TokSemiColon, ";");
  builder.finish_node();

  auto *green_root = builder.finish();
  auto tree = make_tree(std::move(interner), green_root);
  auto root = tree.root();

  REQUIRE(root.text() == source);
  REQUIRE(root.text_len() == source.size());
}

TEST_CASE("SyntaxNode child_node and child_token_text", "[cst]") {
  auto interner = std::make_unique<GreenInterner>();
  TreeBuilder builder(*interner);

  builder.start_node(SyntaxKind::FuncDef);
  builder.token(SyntaxKind::TokFunc, "fn");
  builder.token(SyntaxKind::TokWhitespace, " ");
  {
    builder.start_node(SyntaxKind::Prototype);
    builder.token(SyntaxKind::TokID, "main");
    builder.finish_node();
  }
  builder.finish_node();

  auto *green_root = builder.finish();
  auto tree = make_tree(std::move(interner), green_root);
  auto root = tree.root();

  auto fn_text = root.child_token_text(SyntaxKind::TokFunc);
  REQUIRE(fn_text.has_value());
  REQUIRE(*fn_text == "fn");

  auto proto = root.child_node(SyntaxKind::Prototype);
  REQUIRE(proto.has_value());
  REQUIRE(proto->kind() == SyntaxKind::Prototype);

  auto missing = root.child_node(SyntaxKind::Block);
  REQUIRE_FALSE(missing.has_value());
}

TEST_CASE("SyntaxNode node_at_offset", "[cst]") {
  auto interner = std::make_unique<GreenInterner>();
  TreeBuilder builder(*interner);

  // "fn main"
  builder.start_node(SyntaxKind::SourceFile);
  {
    builder.start_node(SyntaxKind::FuncDef);
    builder.token(SyntaxKind::TokFunc, "fn");
    builder.token(SyntaxKind::TokWhitespace, " ");
    builder.token(SyntaxKind::TokID, "main");
    builder.finish_node();
  }
  builder.finish_node();

  auto *green_root = builder.finish();
  auto tree = make_tree(std::move(interner), green_root);
  auto root = tree.root();

  // Offset 0 should be in FuncDef (at "fn")
  auto node_at_0 = root.node_at_offset(0);
  REQUIRE(node_at_0.kind() == SyntaxKind::FuncDef);

  // Offset 3 should be in FuncDef (at " ")
  auto node_at_3 = root.node_at_offset(3);
  REQUIRE(node_at_3.kind() == SyntaxKind::FuncDef);

  // Offset 4 should be in FuncDef (at "main")
  auto node_at_4 = root.node_at_offset(4);
  REQUIRE(node_at_4.kind() == SyntaxKind::FuncDef);
}

TEST_CASE("SyntaxNode dump", "[cst]") {
  auto interner = std::make_unique<GreenInterner>();
  TreeBuilder builder(*interner);

  builder.start_node(SyntaxKind::SourceFile);
  builder.token(SyntaxKind::TokLet, "let");
  builder.finish_node();

  auto *green_root = builder.finish();
  auto tree = make_tree(std::move(interner), green_root);
  auto root = tree.root();

  std::string dumped = root.dump();
  REQUIRE(dumped.find("SourceFile@0..3") != std::string::npos);
  REQUIRE(dumped.find("TokLet \"let\"@0..3") != std::string::npos);
}

// === CompilerDB tests ===

TEST_CASE("CompilerDB caching", "[cst]") {
  sammine_lang::CompilerDB db;

  SECTION("initial state") {
    REQUIRE(db.revision() == 0);
    REQUIRE_FALSE(db.is_cached());
  }

  SECTION("set_source bumps revision") {
    db.set_source("let x = 1;");
    REQUIRE(db.revision() == 1);
    REQUIRE_FALSE(db.is_cached());

    db.set_source("let x = 2;");
    REQUIRE(db.revision() == 2);
  }

  SECTION("parse caches and returns same result") {
    db.set_source("let x = 42;");
    REQUIRE_FALSE(db.is_cached());

    const auto &tree1 = db.parse();
    REQUIRE(db.is_cached());

    // Second call returns cached result (no re-parse)
    const auto &tree2 = db.parse();
    REQUIRE(&tree1 == &tree2); // Same reference
  }

  SECTION("parse invalidates on new source") {
    db.set_source("let x = 1;");
    db.parse();
    REQUIRE(db.is_cached());

    db.set_source("let x = 2;");
    REQUIRE_FALSE(db.is_cached());

    const auto &tree2 = db.parse();
    REQUIRE(db.is_cached());
    // New tree, different content
    REQUIRE(tree2.root().text() == "let x = 2;");
  }

  SECTION("lossless round-trip through CompilerDB") {
    std::string source = "let x = 42;";
    db.set_source(source);
    const auto &tree = db.parse();
    REQUIRE(tree.root().text() == source);
    REQUIRE(tree.root().text_len() == source.size());
  }
}
