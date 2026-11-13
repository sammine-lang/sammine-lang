#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include <memory>

//! \file SizeClosures.h
//! \brief How many words of bound values a value of each function type
//! carries: the most any closure of that type the module makes binds. A
//! closure carries what it binds, by value (see
//! knowledge_base/plans/closures-by-value.md, stage 1).

namespace sammine_lang {

/// Gives every `!sammine.fn` in `module` its words, read off the module's
/// `sammine.closure`s: the AST is lowered with none, and MIR is whole once
/// this has run. A closure that binds a value of its own type, directly or
/// through other closures, can't carry it, its size would have no end: it
/// counts for one word, a pointer to what it binds in its maker's frame
/// (smn::ClosureOp::bindsInline). The sizes of enums holding function
/// values, and the offsets into their payloads, follow.
void sizeClosures(mlir::ModuleOp module);

/// `sammine-size-closures`: sizeClosures as a pass, for sammine-opt.
std::unique_ptr<mlir::Pass> createSizeClosuresPass();

/// Makes the pass available to tools by its name.
void registerSizeClosuresPass();

} // namespace sammine_lang
