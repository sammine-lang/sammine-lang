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

} // namespace sammine_lang
