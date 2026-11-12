#pragma once

#include "typecheck/Types.h"

#include <cstdint>
#include <vector>

//! \file TypeLayout.h
//! \brief How many bytes a value of a sammine type takes, as enums lay out
//! their payloads: each field at the next multiple of its own size. The
//! module's enum types are sized by it and MIR lays payloads out by it, so
//! an enum is the same bytes in every function and declaration.

namespace sammine_lang::layout {

/// The bytes a value of `type` takes in an enum's payload.
int64_t typeSize(const Type &type);
/// The bytes a variant's payload takes, its fields laid out in order.
int64_t variantPayloadSize(const std::vector<Type> &payload_types);
/// Where the field of `field_type` after `byte_offset` goes; moves
/// `byte_offset` past it.
int64_t advancePayloadOffset(int64_t &byte_offset, const Type &field_type);

} // namespace sammine_lang::layout
