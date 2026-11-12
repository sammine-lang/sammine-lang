#include "cabi/CABI.h"

#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/BuiltinTypes.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

namespace sammine_lang::cabi {

namespace {

// A scalar field of a flattened struct.
struct Field {
  uint64_t offset;
  uint64_t size;
  bool is_float;
};

void flatten(mlir::Type type, uint64_t offset, const mlir::DataLayout &layout,
             llvm::SmallVectorImpl<Field> &fields) {
  if (auto st = mlir::dyn_cast<mlir::LLVM::LLVMStructType>(type)) {
    uint64_t field_offset = 0;
    for (auto field : st.getBody()) {
      field_offset = llvm::alignTo(field_offset,
                                   layout.getTypeABIAlignment(field));
      flatten(field, offset + field_offset, layout, fields);
      field_offset += layout.getTypeSize(field).getFixedValue();
    }
    return;
  }
  if (auto arr = mlir::dyn_cast<mlir::LLVM::LLVMArrayType>(type)) {
    uint64_t elem_size = layout.getTypeSize(arr.getElementType());
    for (uint64_t i = 0; i < arr.getNumElements(); i++)
      flatten(arr.getElementType(), offset + i * elem_size, layout, fields);
    return;
  }
  fields.push_back({offset, layout.getTypeSize(type).getFixedValue(),
                    mlir::isa<mlir::FloatType>(type)});
}

// The register type for the 8-byte chunk starting at `start`: floating point
// if every field in it is, otherwise an integer as wide as its data.
mlir::Type chunkType(llvm::ArrayRef<Field> fields, uint64_t start,
                     mlir::MLIRContext *ctx) {
  bool all_float = true;
  uint64_t end = start;
  for (auto &field : fields) {
    if (field.offset < start || field.offset >= start + 8)
      continue;
    all_float = all_float && field.is_float;
    end = std::max(end, field.offset + field.size);
  }
  uint64_t bytes = end - start;
  if (all_float)
    return bytes > 4 ? mlir::Type(mlir::Float64Type::get(ctx))
                     : mlir::Type(mlir::Float32Type::get(ctx));
  return mlir::IntegerType::get(ctx, static_cast<unsigned>(bytes * 8));
}

} // namespace

mlir::Type registerType(mlir::Type structTy, const mlir::DataLayout &layout) {
  uint64_t size = layout.getTypeSize(structTy).getFixedValue();
  if (size > 16)
    return nullptr;

  llvm::SmallVector<Field> fields;
  flatten(structTy, 0, layout, fields);

  auto *ctx = structTy.getContext();
  auto low = chunkType(fields, 0, ctx);
  if (size <= 8)
    return low;
  return mlir::LLVM::LLVMStructType::getLiteral(ctx,
                                                {low, chunkType(fields, 8, ctx)});
}

Registers registersFor(mlir::Type type) {
  Registers regs;
  if (auto st = mlir::dyn_cast<mlir::LLVM::LLVMStructType>(type)) {
    for (auto field : st.getBody()) {
      auto field_regs = registersFor(field);
      regs.integer += field_regs.integer;
      regs.floating += field_regs.floating;
    }
  } else if (mlir::isa<mlir::FloatType>(type)) {
    regs.floating = 1;
  } else {
    regs.integer = 1;
  }
  return regs;
}

namespace {

// `value` as C passes it through `...`. sammine has no short, so only these
// are promoted.
mlir::Value promoteVariadic(mlir::OpBuilder &builder, mlir::Location loc,
                            mlir::Value value) {
  auto type = value.getType();
  if (type.isF32())
    return mlir::LLVM::FPExtOp::create(builder, loc, builder.getF64Type(),
                                       value);
  if (type.isInteger(8))
    return mlir::LLVM::SExtOp::create(builder, loc, builder.getI32Type(),
                                      value);
  if (type.isInteger(1))
    return mlir::LLVM::ZExtOp::create(builder, loc, builder.getI32Type(),
                                      value);
  return value;
}

} // namespace

mlir::LLVM::GlobalOp createString(mlir::OpBuilder &builder,
                                  mlir::ModuleOp module, mlir::Location loc,
                                  llvm::StringRef name, llvm::StringRef text) {
  std::string bytes = text.str();
  bytes.push_back('\0');
  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(module.getBody());
  auto type = mlir::LLVM::LLVMArrayType::get(builder.getI8Type(), bytes.size());
  return mlir::LLVM::GlobalOp::create(builder, loc, type, /*isConstant=*/true,
                                      mlir::LLVM::Linkage::Internal, name,
                                      builder.getStringAttr(bytes));
}

mlir::LLVM::CallOp emitCall(mlir::OpBuilder &builder, mlir::Location loc,
                            mlir::LLVM::LLVMFuncOp callee,
                            llvm::SmallVector<mlir::Value, 4> args) {
  auto fnTy = callee.getFunctionType();
  if (fnTy.isVarArg())
    for (unsigned i = fnTy.getNumParams(); i < args.size(); i++)
      args[i] = promoteVariadic(builder, loc, args[i]);
  auto call = mlir::LLVM::CallOp::create(builder, loc, callee, args);
  // Without sret and byval on the call too, LLVM's optimizer drops the
  // stores that fill a byval copy.
  if (auto declArgAttrs = callee.getArgAttrsAttr()) {
    llvm::SmallVector<mlir::Attribute> callArgAttrs(declArgAttrs.begin(),
                                                    declArgAttrs.end());
    callArgAttrs.resize(args.size(), builder.getDictionaryAttr({}));
    call.setArgAttrsAttr(builder.getArrayAttr(callArgAttrs));
  }
  return call;
}

} // namespace sammine_lang::cabi
