#pragma once

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
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

/// Whether MIR defines `func`: it has a body, and the body is MIR.
bool definesMir(mlir::FunctionOpInterface func);

/// The attribute on a `sammine.closure` whose value will carry what it binds
/// as a tuple or enum, whatever its type's words are: one that binds a
/// closure of its own function type needn't stay in its maker's frame
/// (LambdaSets).
constexpr llvm::StringLiteral kByValueAttr = "sammine.by_value";

/// The attribute on a `sammine.aggregate` or `sammine.variant` holding a
/// closure's captures, which may hold views lent to the closure
/// (LambdaSets).
constexpr llvm::StringLiteral kCapturesAttr = "sammine.captures";

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
/// The 8-byte words a closure needs to carry a value of `type`.
int64_t wordsFor(mlir::Type type);
/// Where each of `fields` goes in a payload, in order.
llvm::SmallVector<int64_t> payloadOffsets(llvm::ArrayRef<mlir::Type> fields);
/// The bytes a payload of `fields` takes.
int64_t payloadSize(llvm::ArrayRef<mlir::Type> fields);
/// The enum `name` of `variants`, each the tuple of its payload's fields,
/// with room for the largest payload.
EnumType makeEnum(mlir::MLIRContext *ctx, llvm::StringRef name,
                  llvm::ArrayRef<mlir::StringAttr> variantNames,
                  llvm::ArrayRef<mlir::Type> variants);
/// The fields of variant `i`'s payload.
llvm::ArrayRef<mlir::Type> variantFields(EnumType en, uint64_t i);

/// Marks a function the program declared `let unsafe`: what MIR checks of
/// the invariants unsafe code keeps, it checks only elsewhere.
constexpr llvm::StringLiteral kUnsafeAttr = "sammine.unsafe";

/// The function that drops a value of `type`, its Drop instance's method
/// (`Drop<T>::Drop`) in `module`, or null if it has none.
mlir::FunctionOpInterface dropFunction(mlir::ModuleOp module, mlir::Type type);

/// Calls `fn(operand, arg)` for each value `branch` forwards to a successor,
/// with the block argument it becomes.
void forEachBranchEdge(
    mlir::BranchOpInterface branch,
    llvm::function_ref<void(mlir::Value operand, mlir::BlockArgument arg)> fn);

/// The types `type` is built of, in order: a pointer's or a place's pointee,
/// a struct's fields, a tuple's elements, an array's element, an enum's
/// variants, a function type's parameters then results. None for any other
/// type.
llvm::SmallVector<mlir::Type> typeParts(mlir::Type type);
/// `type` built of `parts` instead, in typeParts' order; an enum with room
/// for the largest of its payloads as they are then.
mlir::Type withTypeParts(mlir::Type type, llvm::ArrayRef<mlir::Type> parts);
/// `type` with `fn` applied to each type it is built of, innermost first,
/// and then to what that makes of it. Null if `fn` gives null for any.
mlir::Type mapType(mlir::Type type,
                   llvm::function_ref<mlir::Type(mlir::Type)> fn);
/// Gives each `sammine.variant` and `sammine.payload` under `root` the byte
/// offsets its enum's type has now: after the types of payloads changed.
void refreshPayloadOffsets(mlir::Operation *root);

/// Whether a value of `type` owns memory, which its holder must free or hand
/// on: an owned pointer, or a struct, tuple or enum holding one.
bool ownsMemory(mlir::Type type);

} // namespace sammine_lang::smn

#define GET_OP_CLASSES
#include "SammineOps.h.inc"
