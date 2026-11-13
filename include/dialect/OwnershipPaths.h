#pragma once

#include "mlir/IR/Location.h"
#include "mlir/IR/Value.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>

//! \file OwnershipPaths.h
//! \brief Paths into MIR values and places: the parts of them that own
//! memory, as the ownership analysis follows them, and their names in the
//! source.

namespace sammine_lang::ownership {

/// One step from a value or place to a part of it.
struct Step {
  enum Kind : uint8_t {
    /// Field `a` of a struct or tuple.
    Field,
    /// Field `b` of the payload of variant `a` of an enum.
    Payload,
    /// What an owned pointer points at.
    Pointee,
    /// Any element of an array: the analysis doesn't tell them apart.
    Element,
  };
  Kind kind;
  int64_t a = 0, b = 0;
  auto operator<=>(const Step &) const = default;
};

/// A part of a value that owns memory, or of a place: a root, a value an op
/// makes or a binding's slot, and the steps to the part.
struct Path {
  mlir::Value root;
  llvm::SmallVector<Step, 2> steps;

  Path child(Step step) const {
    Path path = *this;
    path.steps.push_back(step);
    return path;
  }
  /// This path without its last step: what the part is a part of.
  Path parent() const { return Path{root, {steps.begin(), steps.end() - 1}}; }
  /// Whether `other` is this path or a part of it.
  bool contains(const Path &other) const {
    return root == other.root && steps.size() <= other.steps.size() &&
           std::equal(steps.begin(), steps.end(), other.steps.begin());
  }
  /// `other`, a part of this path, as the same part of `to`.
  Path moveTo(const Path &other, const Path &to) const {
    Path moved = to;
    moved.steps.append(other.steps.begin() + steps.size(), other.steps.end());
    return moved;
  }
  /// Whether the last step is to what a pointer points at.
  bool isPointee() const {
    return !steps.empty() && steps.back().kind == Step::Pointee;
  }
  bool operator==(const Path &other) const {
    return root == other.root && steps == other.steps;
  }
  bool operator<(const Path &other) const {
    if (root != other.root)
      return root.getAsOpaquePointer() < other.root.getAsOpaquePointer();
    return std::lexicographical_compare(steps.begin(), steps.end(),
                                        other.steps.begin(), other.steps.end());
  }
};

/// The type of what `path` names: a value's, or what a place holds.
mlir::Type typeOf(const Path &path);

/// Whether `type` is a pointer that owns its memory: a box or a buffer.
bool isOwnedPtr(mlir::Type type);

/// The parts of a `type` that own memory, one step down. Only a box's
/// pointee is one: a buffer's values are its owner's to count.
llvm::SmallVector<Step> partsOf(mlir::Type type);

/// The path a value or place names: a value that owns memory, a part of
/// one, a slot, a part of a place. Nullopt for one the analysis doesn't
/// follow: owning nothing, or behind a pointer that doesn't own.
std::optional<Path> pathOf(mlir::Value value);

/// Where `value` is made: its op's location, or its block argument's.
mlir::Location definedAt(mlir::Value value);

/// A binding's name, for a root a binding makes; a value moved out of a
/// place goes by the place's.
std::optional<std::string> bindingName(mlir::Value root);

/// `path` as the source writes it: `b.data`, `*p.a`, `o.i.a`; empty for a
/// value no binding names.
std::string nameOf(const Path &path);

} // namespace sammine_lang::ownership
