#pragma once

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringSet.h"

#include <map>
#include <memory>
#include <string>

//! \file LambdaSets.h
//! \brief Defunctionalization by lambda sets (Brandon et al., "Better
//! Defunctionalization through Lambda Set Specialization", PLDI 2023): each
//! function value's set of possible closures is found by unifying along how
//! values move, and a value of a known set becomes what its closure binds,
//! a tuple for one closure or an enum for several, so a call through it is
//! a direct call, or a switch of them. See
//! knowledge_base/plans/closures-by-value.md, stage 2. Each higher-order
//! function is first copied per set it is used with (specializeLambdaSets).
//!
//! A set reaching code the module doesn't define (C, another module's), a
//! library's own exported function, or a set that holds itself through what
//! its closures bind (the paper's μ sets) stays a `!sammine.fn`, as does
//! every set its closures bind.

namespace sammine_lang {

/// Marks each closure whose set becomes a tuple or an enum
/// (smn::kByValueAttr).
/// Run before the checks, which read it.
void markLambdaSets(mlir::ModuleOp module);

/// Whether `type` has a function type in it: is one, or holds, points to or
/// takes or gives one.
bool holdsFunctionValue(mlir::Type type);

/// For each function MIR defines, what its signature's function values may
/// be, by the functions their closures call and what those bind. Two
/// functions of one description take and give alike closures. Calls and
/// closures of the functions `opaque` names are followed no further than
/// what they are given: what those functions do with it isn't counted.
std::map<mlir::Operation *, std::string>
describeSignatures(mlir::ModuleOp module, const llvm::StringSet<> &opaque = {});

/// Copies each private SCC of the call graph with a function taking or
/// giving function values, whole, per place that calls one of its functions
/// or makes a closure of it, then merges the copies whose signatures are
/// described alike (the paper's specialization, per SCC). Run after the
/// checks, before lowerLambdaSets, whose sets are then one per copy.
mlir::LogicalResult specializeLambdaSets(mlir::ModuleOp module);

/// `sammine-specialize-lambda-sets`: specializeLambdaSets as a pass.
std::unique_ptr<mlir::Pass> createSpecializeLambdaSetsPass();
void registerSpecializeLambdaSetsPass();

/// Rewrites each function value of a known set into what its closures bind,
/// and each call through one into direct calls. Run after the checks,
/// before sammine-mir-to-llvm.
mlir::LogicalResult lowerLambdaSets(mlir::ModuleOp module);

/// `sammine-lower-lambda-sets`: lowerLambdaSets as a pass, for sammine-opt.
std::unique_ptr<mlir::Pass> createLowerLambdaSetsPass();

/// Makes the pass available to tools by its name.
void registerLowerLambdaSetsPass();

} // namespace sammine_lang
