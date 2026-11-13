#pragma once

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include <string>

#include "SammineDialect.h.inc"

#include "SammineEnums.h.inc"

#define GET_TYPEDEF_CLASSES
#include "SammineTypes.h.inc"

namespace sammine_lang::smn {
/// Marks a function whose body is MIR, as opposed to one only declared (of
/// another module, a C binding, a kernel's wrapper). Its signature has MIR's
/// types; a declaration's, LLVM's.
constexpr llvm::StringLiteral kMirAttr = "sammine.mir";

/// Marks the argument a call passes a buffer for its callee's result in: a
/// kernel's wrapper fills one it is passed last with an array it returns.
constexpr llvm::StringLiteral kResultBufferAttr = "sammine.result_buffer";

/// Why an op takes the values it is given, where its kind doesn't say: a
/// call made for `x as T` converts x. For the ownership check's messages.
constexpr llvm::StringLiteral kReasonAttr = "sammine.reason";

/// Whether a value of `type` owns memory, which its holder must free or hand
/// on: an owned pointer, or a struct, tuple or enum holding one.
bool ownsMemory(mlir::Type type);

/// `type` as sammine writes it, for messages: `'ptr<i32>`, `vec::Vec<i32>`.
/// MIR's integers have no sign, so `u32` reads as `i32`.
std::string spelling(mlir::Type type);
} // namespace sammine_lang::smn

#define GET_OP_CLASSES
#include "SammineOps.h.inc"
