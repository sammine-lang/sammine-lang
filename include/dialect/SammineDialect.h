#pragma once

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

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

/// The types of the fields of a struct or tuple, or nullopt for any other
/// type.
std::optional<llvm::ArrayRef<mlir::Type>> fieldTypes(mlir::Type type);

/// How enums lay out their payloads: each field at the next multiple of its
/// own size, in the bytes `sizeOf` gives it. VariantOp and PayloadOp carry
/// the offsets; an enum's type, the size of its largest payload.
int64_t sizeOf(mlir::Type type);
/// Where each of `fields` goes in a payload, in order.
llvm::SmallVector<int64_t> payloadOffsets(llvm::ArrayRef<mlir::Type> fields);
/// The bytes a payload of `fields` takes.
int64_t payloadSize(llvm::ArrayRef<mlir::Type> fields);

/// Whether a value of `type` owns memory, which its holder must free or hand
/// on: an owned pointer, or a struct, tuple or enum holding one.
bool ownsMemory(mlir::Type type);

} // namespace sammine_lang::smn

#define GET_OP_CLASSES
#include "SammineOps.h.inc"
