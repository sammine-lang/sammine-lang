#pragma once

#include "lex/Token.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Value.h"

namespace sammine_lang {

/// Emit an integer arithmetic/bitwise op. Returns nullptr if the token
/// is not a recognized integer operator.
mlir::Value emitIntArithOp(mlir::OpBuilder &builder, mlir::Location location,
                           mlir::Value lhs, mlir::Value rhs, TokenType op,
                           bool is_unsigned);

/// Emit a float arithmetic op. Returns nullptr if the token is not a
/// recognized float operator.
mlir::Value emitFloatArithOp(mlir::OpBuilder &builder, mlir::Location location,
                             mlir::Value lhs, mlir::Value rhs, TokenType op);

/// Convert `value` to the integer or float type `to`. The signedness of each
/// side decides how an integer is widened or turned into a float, and the
/// reverse. A float saturates to an integer: NaN gives 0, and a value out of
/// range gives the integer type's min or max.
mlir::Value emitNumberConversion(mlir::OpBuilder &builder,
                                 mlir::Location location, mlir::Value value,
                                 mlir::Type to, bool from_unsigned,
                                 bool to_unsigned);

} // namespace sammine_lang
