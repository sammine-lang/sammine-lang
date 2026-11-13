#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LogicalResult.h"

#include <memory>

//! \file LambdaSets.h
//! \brief Defunctionalization by lambda sets (Brandon et al., "Better
//! Defunctionalization through Lambda Set Specialization", PLDI 2023): each
//! function value's set of possible closures is found by unifying along how
//! values move, and a value of a known set becomes what its closure binds,
//! a tuple for one closure or an enum for several, so a call through it is
//! a direct call, or a switch of them. See
//! knowledge_base/plans/closures-by-value.md, stage 2.
//!
//! A set reaching code the module doesn't define (C, another module's), a
//! library's own exported function, or a set that holds itself through what
//! its closures bind (the paper's μ sets) stays a `!sammine.fn`, as does
//! every set its closures bind.

namespace sammine_lang {

/// The attribute on a `sammine.closure` whose value will carry what it binds
/// as a tuple or enum, whatever its type's words are: one that binds a
/// closure of its own function type needn't stay in its maker's frame.
constexpr llvm::StringLiteral kByValueAttr = "sammine.by_value";

/// The attribute on a `sammine.aggregate` or `sammine.variant` holding a
/// closure's captures, which may hold views lent to the closure.
constexpr llvm::StringLiteral kCapturesAttr = "sammine.captures";

/// Marks each closure whose set becomes a tuple or an enum (kByValueAttr).
/// Run before the checks, which read it.
void markLambdaSets(mlir::ModuleOp module);

/// Rewrites each function value of a known set into what its closures bind,
/// and each call through one into direct calls. Run after the checks,
/// before sammine-mir-to-llvm.
mlir::LogicalResult lowerLambdaSets(mlir::ModuleOp module);

/// `sammine-lower-lambda-sets`: lowerLambdaSets as a pass, for sammine-opt.
std::unique_ptr<mlir::Pass> createLowerLambdaSetsPass();

/// Makes the pass available to tools by its name.
void registerLowerLambdaSetsPass();

} // namespace sammine_lang
