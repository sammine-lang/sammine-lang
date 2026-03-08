# MLIR Linalg + GPU Lowering C++ API Reference

Research notes for Phase 7 (tensor types + linalg) and future GPU lowering.

---

## 1. Creating Tensor Types in C++

```cpp
#include "mlir/IR/BuiltinTypes.h"   // RankedTensorType
#include "mlir/IR/Builders.h"       // OpBuilder

// Scalar element types
mlir::Type f64Type = builder.getF64Type();
mlir::Type f32Type = builder.getF32Type();
mlir::Type i64Type = builder.getI64Type();

// 1-D tensor: tensor<3xf64>
auto tensor1d = mlir::RankedTensorType::get({3}, f64Type);

// 2-D tensor: tensor<4x5xf64>
auto tensor2d = mlir::RankedTensorType::get({4, 5}, f64Type);

// Dynamic dimension: tensor<?xf64>
auto tensorDyn = mlir::RankedTensorType::get(
    {mlir::ShapedType::kDynamic}, f64Type);

// 0-D tensor (scalar wrapped in tensor): tensor<f64>
auto tensor0d = mlir::RankedTensorType::get({}, f64Type);
```

### Creating an empty tensor (for output operand)

```cpp
#include "mlir/Dialect/Tensor/IR/Tensor.h"  // tensor::EmptyOp

// Static shape: tensor.empty() : tensor<3xf64>
mlir::Value emptyTensor = tensor::EmptyOp::create(
    builder, loc, /*shape=*/{3}, f64Type);

// Dynamic shape: tensor.empty(%size) : tensor<?xf64>
mlir::Value size = ...;  // some index-typed SSA value
mlir::Value emptyDyn = tensor::EmptyOp::create(
    builder, loc, /*shape=*/ArrayRef<int64_t>{mlir::ShapedType::kDynamic},
    f64Type, /*dynamicSizes=*/ValueRange{size});
```

---

## 2. Creating `linalg.map` (linalg::MapOp)

### MLIR IR Syntax

```mlir
// Unary map (e.g. negate):
%result = linalg.map ins(%input : tensor<3xf64>)
                     outs(%init : tensor<3xf64>)
  (%in: f64) {
    %neg = arith.negf %in : f64
    linalg.yield %neg : f64
  }

// Binary map (e.g. add):
%result = linalg.map ins(%lhs : tensor<3xf64>, %rhs : tensor<3xf64>)
                     outs(%init : tensor<3xf64>)
  (%l: f64, %r: f64) {
    %sum = arith.addf %l, %r : f64
    linalg.yield %sum : f64
  }

// Zero-input map (e.g. fill with linalg.index):
%result = linalg.map outs(%init : tensor<3xindex>)
  () {
    %idx = linalg.index 0 : index
    linalg.yield %idx : index
  }
```

### Semantics
- **N-ary elementwise**: input shapes must all match the output shape.
- **No broadcasting**: unlike numpy, shapes must be identical.
- **Block arguments**: one per input (NOT per output, unlike `linalg.generic`).
- **linalg.yield**: terminates the body, yields the scalar result.
- Implements `LinalgStructuredInterface` and `DestinationStyleOpInterface`.

### C++ Builder API

```cpp
#include "mlir/Dialect/Linalg/IR/Linalg.h"  // linalg::MapOp, linalg::YieldOp

// MapOp::build signature:
// void MapOp::build(
//     OpBuilder &builder, OperationState &result,
//     ValueRange inputs,           // input tensors (or memrefs)
//     Value init,                  // output/init tensor
//     function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuild,
//     ArrayRef<NamedAttribute> attributes = {});

// Example: element-wise add of two tensor<3xf64>
mlir::Value lhs = ...;   // tensor<3xf64>
mlir::Value rhs = ...;   // tensor<3xf64>
mlir::Value init = ...;  // tensor<3xf64> (e.g. from tensor.empty)

auto mapOp = linalg::MapOp::create(
    builder, loc,
    /*inputs=*/ValueRange{lhs, rhs},
    /*init=*/init,
    /*bodyBuild=*/[](OpBuilder &b, Location loc, ValueRange args) {
      // args[0] = scalar from lhs (f64)
      // args[1] = scalar from rhs (f64)
      mlir::Value sum = arith::AddFOp::create(b, loc, args[0], args[1]);
      linalg::YieldOp::create(b, loc, ValueRange{sum});
    });

mlir::Value result = mapOp.getResult(0);  // tensor<3xf64>
```

### How the body region works internally

The `MapOp::build` method calls `buildGenericRegion()` which:
1. Creates a `Block` in the op's region.
2. Adds one `BlockArgument` per input, typed as the **element type** (e.g. `f64`, not `tensor<3xf64>`).
3. Calls your `bodyBuild` lambda with those block arguments.
4. Your lambda must terminate with `linalg::YieldOp`.

```cpp
// Internal implementation (simplified from LinalgOps.cpp):
static void buildGenericRegion(
    OpBuilder &builder, Location loc, Region &region,
    ValueRange inputs, ValueRange outputs,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuild) {
  SmallVector<Type> blockArgTypes;
  SmallVector<Location> blockArgLocs;
  for (ValueRange container : {inputs, outputs}) {
    for (Value v : container) {
      Type t = v.getType();
      blockArgTypes.push_back(
          isa<MemRefType, RankedTensorType>(t) ? getElementTypeOrSelf(t) : t);
      blockArgLocs.push_back(v.getLoc());
    }
  }
  OpBuilder::InsertionGuard guard(builder);
  Block *body = builder.createBlock(&region, region.end(),
                                     blockArgTypes, blockArgLocs);
  bodyBuild(builder, loc, body->getArguments());
}
```

---

## 3. Creating `linalg.reduce` (linalg::ReduceOp)

### MLIR IR Syntax

```mlir
// Sum-reduce a 1-D tensor along dimension 0:
%sum = linalg.reduce ins(%input : tensor<3xf64>)
                     outs(%identity : tensor<f64>)
                     dimensions = [0]
  (%in: f64, %acc: f64) {
    %add = arith.addf %in, %acc : f64
    linalg.yield %add : f64
  }

// Max-reduce a 2-D tensor along dimension 1:
%max = linalg.reduce ins(%matrix : tensor<4x5xf32>)
                     outs(%neg_inf : tensor<4xf32>)
                     dimensions = [1]
  (%in: f32, %acc: f32) {
    %m = arith.maximumf %in, %acc : f32
    linalg.yield %m : f32
  }

// Reduce along multiple dimensions:
%total = linalg.reduce ins(%tensor3d : tensor<2x3x4xf64>)
                       outs(%identity : tensor<2xf64>)
                       dimensions = [1, 2]
  (%in: f64, %acc: f64) {
    %add = arith.addf %in, %acc : f64
    linalg.yield %add : f64
  }
```

### Semantics
- **dimensions attribute**: specifies which dimensions are collapsed/reduced.
- **Block arguments**: `%in` = element from input, `%acc` = accumulator from output.
- **Output shape**: input shape with the reduced dimensions removed.
  - `tensor<4x5xf32>` reduced along `[1]` -> `tensor<4xf32>`
  - `tensor<2x3x4xf64>` reduced along `[1,2]` -> `tensor<2xf64>`
  - `tensor<3xf64>` reduced along `[0]` -> `tensor<f64>` (scalar tensor)
- **Identity element**: the `outs` tensor must be pre-filled with the reduction identity (e.g. 0.0 for sum, -inf for max).

### C++ Builder API

```cpp
#include "mlir/Dialect/Linalg/IR/Linalg.h"

// ReduceOp::build signature:
// void ReduceOp::build(
//     OpBuilder &builder, OperationState &result,
//     ValueRange inputs,             // input tensors
//     ValueRange inits,              // output/accumulator tensors
//     ArrayRef<int64_t> dimensions,  // which dims to reduce
//     function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuild,
//     ArrayRef<NamedAttribute> attributes = {});

// Example: sum-reduce tensor<3xf64> along dim 0
mlir::Value input = ...;    // tensor<3xf64>
mlir::Value identity = ...; // tensor<f64> pre-filled with 0.0

auto reduceOp = linalg::ReduceOp::create(
    builder, loc,
    /*inputs=*/ValueRange{input},
    /*inits=*/ValueRange{identity},
    /*dimensions=*/ArrayRef<int64_t>{0},
    /*bodyBuild=*/[](OpBuilder &b, Location loc, ValueRange args) {
      // args[0] = scalar element from input (f64)
      // args[1] = scalar accumulator from output (f64)
      mlir::Value sum = arith::AddFOp::create(b, loc, args[0], args[1]);
      linalg::YieldOp::create(b, loc, ValueRange{sum});
    });

mlir::Value result = reduceOp.getResult(0);  // tensor<f64>
```

### Pre-filling the identity element

```cpp
// For sum reduction: fill with 0.0
mlir::Value zero = arith::ConstantOp::create(
    builder, loc, builder.getF64FloatAttr(0.0));
mlir::Value emptyScalar = tensor::EmptyOp::create(
    builder, loc, /*shape=*/{}, f64Type);
mlir::Value identity = linalg::FillOp::create(
    builder, loc, ValueRange{zero}, ValueRange{emptyScalar})
    .getResult(0);

// For max reduction: fill with -inf
mlir::Value negInf = arith::ConstantOp::create(
    builder, loc,
    builder.getF64FloatAttr(-std::numeric_limits<double>::infinity()));
```

---

## 4. `linalg.yield` — Body Terminator

```cpp
#include "mlir/Dialect/Linalg/IR/Linalg.h"

// linalg.yield terminates the body of any linalg structured op.
// It's an implicit terminator (SingleBlockImplicitTerminator<YieldOp>).

// Inside a bodyBuild lambda:
linalg::YieldOp::create(builder, loc, ValueRange{resultValue});

// The yielded value's type must match the element type of the output tensor.
// For map:  yield 1 value (the mapped result)
// For reduce: yield 1 value (the updated accumulator)
```

---

## 5. Arith Operations for Body Blocks

```cpp
#include "mlir/Dialect/Arith/IR/Arith.h"

// Inside a bodyBuild lambda:
// Floating-point operations:
mlir::Value add  = arith::AddFOp::create(b, loc, lhs, rhs);
mlir::Value sub  = arith::SubFOp::create(b, loc, lhs, rhs);
mlir::Value mul  = arith::MulFOp::create(b, loc, lhs, rhs);
mlir::Value div  = arith::DivFOp::create(b, loc, lhs, rhs);
mlir::Value neg  = arith::NegFOp::create(b, loc, operand);
mlir::Value maxf = arith::MaximumFOp::create(b, loc, lhs, rhs);

// Integer operations:
mlir::Value addi = arith::AddIOp::create(b, loc, lhs, rhs);
mlir::Value muli = arith::MulIOp::create(b, loc, lhs, rhs);

// Constants:
mlir::Value cst  = arith::ConstantOp::create(b, loc, b.getF64FloatAttr(42.0));
mlir::Value zero = arith::ConstantOp::create(b, loc, b.getF64FloatAttr(0.0));
```

---

## 6. Required Headers

### For linalg.map / linalg.reduce emission

```cpp
// Core MLIR
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"        // RankedTensorType
#include "mlir/IR/MLIRContext.h"

// Dialects
#include "mlir/Dialect/Linalg/IR/Linalg.h"          // MapOp, ReduceOp, YieldOp, FillOp
#include "mlir/Dialect/Tensor/IR/Tensor.h"           // tensor::EmptyOp
#include "mlir/Dialect/Arith/IR/Arith.h"             // arith::AddFOp, etc.
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"  // bufferization dialect

// For lowering passes
#include "mlir/Dialect/Linalg/Passes.h"              // convert-linalg-to-loops, etc.
#include "mlir/Dialect/Bufferization/Transforms/Passes.h" // one-shot-bufferize
#include "mlir/Conversion/Passes.h"                   // all conversion passes (umbrella)
```

### For GPU lowering passes (future)

```cpp
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/GPU/Transforms/Passes.h"       // gpu-kernel-outlining, gpu-map-parallel-loops
#include "mlir/Conversion/SCFToGPU/SCFToGPUPass.h"    // convert-parallel-loops-to-gpu
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"  // convert-gpu-to-nvvm
#include "mlir/Conversion/GPUCommon/GPUCommonPass.h"   // gpu-to-llvm
#include "mlir/Dialect/SCF/IR/SCF.h"                   // scf.parallel
```

### Dialect registration (in MLIRGen or context setup)

```cpp
// Register all needed dialects on the MLIRContext:
mlir::DialectRegistry registry;
registry.insert<mlir::linalg::LinalgDialect>();
registry.insert<mlir::tensor::TensorDialect>();
registry.insert<mlir::bufferization::BufferizationDialect>();
registry.insert<mlir::arith::ArithDialect>();
registry.insert<mlir::func::FuncDialect>();
registry.insert<mlir::scf::SCFDialect>();
registry.insert<mlir::memref::MemRefDialect>();
// For GPU (future):
// registry.insert<mlir::gpu::GPUDialect>();
context.appendDialectRegistry(registry);
```

---

## 7. CMake Libraries to Link

### For linalg + tensor + bufferization (Phase 7)

```cmake
# Add to src/codegen/CMakeLists.txt LINK_LIBS:
target_link_libraries(MLIRBackend PUBLIC
  # Already linked (Phase 1-2):
  MLIRArithDialect
  MLIRFuncDialect
  MLIRSCFDialect
  MLIRMemRefDialect
  MLIRArithToLLVM
  MLIRSCFToControlFlow
  MLIRFuncToLLVM
  MLIRMemRefToLLVM
  MLIRControlFlowToLLVM
  MLIRReconcileUnrealizedCasts

  # NEW for Phase 7:
  MLIRLinalgDialect          # linalg::MapOp, ReduceOp, GenericOp, YieldOp, FillOp
  MLIRLinalgTransforms       # convert-linalg-to-loops, convert-linalg-to-parallel-loops
  MLIRTensorDialect          # tensor::EmptyOp
  MLIRBufferizationDialect   # bufferization dialect IR
  MLIRBufferizationTransforms # one-shot-bufferize pass
  MLIRBufferizationToMemRef  # bufferization.to_memref lowering
)
```

### For GPU lowering (future)

```cmake
# Additional CMake libs for GPU pipeline:
  MLIRGPUDialect             # gpu dialect IR
  MLIRGPUTransforms          # gpu-kernel-outlining, gpu-map-parallel-loops
  MLIRGPUToNVVMTransforms    # convert-gpu-to-nvvm
  MLIRGPUToLLVMIRTranslation # gpu module translation
  MLIRSCFToGPU               # convert-parallel-loops-to-gpu (via SCF)
  MLIRNVVMDialect            # NVVM dialect
  MLIRNVVMToLLVM             # convert-nvvm-to-llvm
```

---

## 8. Lowering Pipeline (Phase 7)

### Tensor -> Loops -> LLVM (CPU path)

```cpp
// In MLIRLowering.cpp — extend the existing PassManager:
mlir::PassManager pm(&context);

// --- Phase 7 passes (tensor/linalg → memref/loops) ---
// 1. Bufferize: tensor -> memref
pm.addPass(mlir::bufferization::createOneShotBufferizePass());

// 2. Lower linalg ops to loop nests
pm.addPass(mlir::createConvertLinalgToLoopsPass());
// NOTE: may need to run twice if bufferization introduces new linalg ops
// pm.addPass(mlir::createConvertLinalgToLoopsPass());

// 3. Lower affine maps to standard ops (if any affine ops remain)
pm.addPass(mlir::createLowerAffinePass());

// --- Existing Phase 1-2 passes (memref/scf/func → LLVM) ---
// 4. SCF -> CF
pm.addPass(mlir::createSCFToControlFlowPass());
// 5. Arith -> LLVM
pm.addPass(mlir::createArithToLLVMConversionPass());
// 6. MemRef -> LLVM
pm.addPass(mlir::createFinalizeMemRefToLLVMConversionPass());
// 7. CF -> LLVM
pm.addPass(mlir::createConvertControlFlowToLLVMPass());
// 8. Func -> LLVM
pm.addPass(mlir::createConvertFuncToLLVMPass());
// 9. Reconcile casts
pm.addPass(mlir::createReconcileUnrealizedCastsPass());

if (failed(pm.run(module)))
  return failure();
```

### Alternative: Linalg to parallel loops (enables GPU path later)

```cpp
// Replace step 2 with:
pm.addPass(mlir::createConvertLinalgToParallelLoopsPass());
// This produces scf.parallel instead of scf.for, enabling GPU mapping later.
```

---

## 9. GPU Lowering Pipeline (Future Reference)

### Full pass pipeline (linalg on tensors -> GPU kernel)

```cpp
mlir::PassManager pm(&context);

// 1. Canonicalize
pm.addPass(mlir::createCanonicalizerPass());

// 2. Bufferize: tensor -> memref
pm.addPass(mlir::bufferization::createOneShotBufferizePass());
// Options: bufferize-function-boundaries, function-boundary-type-conversion=identity-layout-map

// 3. Canonicalize again
pm.addPass(mlir::createCanonicalizerPass());

// 4. Linalg -> parallel loops (scf.parallel)
pm.addPass(mlir::createConvertLinalgToParallelLoopsPass());

// 5. Map parallel loops to GPU dimensions
pm.addPass(mlir::createGpuMapParallelLoopsPass());

// 6. Convert parallel loops -> gpu.launch
pm.addPass(mlir::createParallelLoopToGpuPass());

// 7. Outline gpu.launch body -> gpu.func kernel
pm.addPass(mlir::createGpuKernelOutliningPass());

// 8. Lower GPU ops to NVVM (inside gpu.module)
pm.addNestedPass<mlir::gpu::GPUModuleOp>(
    mlir::createConvertGpuOpsToNVVMOps());
// or: pm.addPass("gpu.module(convert-gpu-to-nvvm)")

// 9. Attach NVVM target for serialization
// pm.addPass(mlir::createNVVMAttachTarget({chip, features}));

// 10. Lower NVVM -> LLVM
pm.addPass(mlir::createConvertNVVMToLLVMPass());

// 11. Lower remaining GPU host code -> LLVM
pm.addPass(mlir::createGpuToLLVMConversionPass());

// 12. Reconcile casts
pm.addPass(mlir::createReconcileUnrealizedCastsPass());

// 13. Serialize GPU modules to binaries (PTX/cubin)
// pm.addPass(mlir::gpu::createGpuModuleToBinaryPass());
```

### Alternative: Affine-based GPU path

```python
# From Stephen Diehl's tutorial (Python PassManager equivalent):
pm.add("one-shot-bufferize{...}")
pm.add("convert-linalg-to-affine-loops")
pm.add("func.func(affine-loop-invariant-code-motion)")
pm.add("func.func(convert-affine-for-to-gpu)")
pm.add("gpu-kernel-outlining")
pm.add("lower-affine")
pm.add("gpu-decompose-memrefs")
pm.add("gpu.module(convert-gpu-to-nvvm{...})")
pm.add("nvvm-attach-target{chip=sm_80 ...}")
pm.add("gpu-to-llvm{use-bare-pointers-for-host ...}")
```

### GPU Pass Reference Table

| Pass CLI name | C++ creation function | Header | CMake library |
|---|---|---|---|
| `one-shot-bufferize` | `bufferization::createOneShotBufferizePass()` | `mlir/Dialect/Bufferization/Transforms/Passes.h` | `MLIRBufferizationTransforms` |
| `convert-linalg-to-loops` | `createConvertLinalgToLoopsPass()` | `mlir/Dialect/Linalg/Passes.h` | `MLIRLinalgTransforms` |
| `convert-linalg-to-parallel-loops` | `createConvertLinalgToParallelLoopsPass()` | `mlir/Dialect/Linalg/Passes.h` | `MLIRLinalgTransforms` |
| `gpu-map-parallel-loops` | `createGpuMapParallelLoopsPass()` | `mlir/Dialect/GPU/Transforms/Passes.h` | `MLIRGPUTransforms` |
| `convert-parallel-loops-to-gpu` | `createParallelLoopToGpuPass()` | `mlir/Conversion/SCFToGPU/SCFToGPUPass.h` | `MLIRSCFToGPU` |
| `gpu-kernel-outlining` | `createGpuKernelOutliningPass()` | `mlir/Dialect/GPU/Transforms/Passes.h` | `MLIRGPUTransforms` |
| `convert-gpu-to-nvvm` | `createConvertGpuOpsToNVVMOps()` | `mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h` | `MLIRGPUToNVVMTransforms` |
| `gpu-to-llvm` | `createGpuToLLVMConversionPass()` | `mlir/Conversion/GPUCommon/GPUCommonPass.h` | `MLIRGPUToLLVMIRTranslation` |
| `convert-nvvm-to-llvm` | `createConvertNVVMToLLVMPass()` | `mlir/Conversion/NVVMToLLVM/NVVMToLLVM.h` | `MLIRNVVMToLLVM` |
| `lower-affine` | `createLowerAffinePass()` | `mlir/Conversion/AffineToStandard/AffineToStandard.h` | `MLIRAffineToStandard` |

---

## 10. Complete Example: Map + Reduce in C++

Putting it all together -- emitting `sum(map(x, fn))` where `fn` squares each element:

```cpp
// Input: tensor<3xf64>
// Step 1: map (square each element)
// Step 2: reduce (sum all elements)

mlir::Type f64 = builder.getF64Type();
auto inputType = RankedTensorType::get({3}, f64);   // tensor<3xf64>
auto scalarType = RankedTensorType::get({}, f64);   // tensor<f64>

// Assume %input is already an mlir::Value of type tensor<3xf64>
mlir::Value input = ...;

// --- Step 1: linalg.map (square) ---
mlir::Value mapInit = tensor::EmptyOp::create(builder, loc, {3}, f64);
auto mapOp = linalg::MapOp::create(
    builder, loc,
    /*inputs=*/ValueRange{input},
    /*init=*/mapInit,
    [](OpBuilder &b, Location loc, ValueRange args) {
      mlir::Value squared = arith::MulFOp::create(b, loc, args[0], args[0]);
      linalg::YieldOp::create(b, loc, ValueRange{squared});
    });
mlir::Value squared = mapOp.getResult(0);  // tensor<3xf64>

// --- Step 2: linalg.reduce (sum) ---
mlir::Value zeroScalar = arith::ConstantOp::create(
    builder, loc, builder.getF64FloatAttr(0.0));
mlir::Value reduceInit = tensor::EmptyOp::create(builder, loc, {}, f64);
mlir::Value identity = linalg::FillOp::create(
    builder, loc, ValueRange{zeroScalar}, ValueRange{reduceInit})
    .getResult(0);

auto reduceOp = linalg::ReduceOp::create(
    builder, loc,
    /*inputs=*/ValueRange{squared},
    /*inits=*/ValueRange{identity},
    /*dimensions=*/ArrayRef<int64_t>{0},
    [](OpBuilder &b, Location loc, ValueRange args) {
      mlir::Value sum = arith::AddFOp::create(b, loc, args[0], args[1]);
      linalg::YieldOp::create(b, loc, ValueRange{sum});
    });
mlir::Value total = reduceOp.getResult(0);  // tensor<f64>

// To extract scalar from tensor<f64>:
// mlir::Value scalar = tensor::ExtractOp::create(builder, loc, total, ValueRange{});
```

### Expected MLIR output

```mlir
%empty = tensor.empty() : tensor<3xf64>
%mapped = linalg.map ins(%input : tensor<3xf64>)
                     outs(%empty : tensor<3xf64>)
  (%in: f64) {
    %sq = arith.mulf %in, %in : f64
    linalg.yield %sq : f64
  }

%zero = arith.constant 0.0 : f64
%scalar_empty = tensor.empty() : tensor<f64>
%identity = linalg.fill ins(%zero : f64) outs(%scalar_empty : tensor<f64>) -> tensor<f64>

%sum = linalg.reduce ins(%mapped : tensor<3xf64>)
                     outs(%identity : tensor<f64>)
                     dimensions = [0]
  (%in: f64, %acc: f64) {
    %add = arith.addf %in, %acc : f64
    linalg.yield %add : f64
  }
```

---

## 11. Key Gotchas

1. **MapOp block args**: One per *input* only (not outputs). `args[0..N-1]` correspond to `inputs[0..N-1]`. This differs from `linalg.generic` which includes output args.

2. **ReduceOp block args**: `args[0..N-1]` from inputs, `args[N..2N-1]` from accumulators/outputs.

3. **tensor.empty is required**: Linalg ops are "destination-style" -- they need an output tensor even if the values are overwritten. Use `tensor.empty` to create it.

4. **Identity for reduce**: The `outs` tensor for ReduceOp must be pre-filled with the identity element. Use `linalg.fill` or `arith.constant` + broadcast.

5. **Bufferization is mandatory**: Tensor semantics are SSA-based (no in-place mutation). `one-shot-bufferize` converts tensor ops to memref ops before lowering to loops/LLVM.

6. **Double linalg-to-loops**: The `convert-linalg-to-loops` pass may need to run twice -- bufferization can introduce new `linalg.map` ops (e.g. from `tensor.generate` lowering).

7. **Op::create vs builder.create**: Modern MLIR (post-2024) prefers `OpType::create(builder, loc, ...)` over `builder.create<OpType>(loc, ...)`. Both work, but `::create` is the preferred pattern going forward.

---

## Sources

- [MLIR Linalg Dialect Documentation](https://mlir.llvm.org/docs/Dialects/Linalg/)
- [RFC: Primitive Ops (MapOp, ReductionOp, TransposeOp, BroadcastOp)](https://discourse.llvm.org/t/rfc-primitive-ops-add-mapop-reductionop-transposeop-broadcastop-to-linalg/64184)
- [LinalgOps.cpp Source (MapOp::build, ReduceOp::build)](https://mlir.llvm.org/doxygen/LinalgOps_8cpp_source.html)
- [MLIR Bufferization Documentation](https://mlir.llvm.org/docs/Bufferization/)
- [MLIR Passes Reference](https://mlir.llvm.org/docs/Passes/)
- [MLIR GPU Dialect Documentation](https://mlir.llvm.org/docs/Dialects/GPU/)
- [GPU Compilation with MLIR (Stephen Diehl)](https://www.stephendiehl.com/posts/mlir_gpu/)
- [Linalg.map Lowering Discussion](https://discourse.llvm.org/t/how-to-lower-linalg-map/70448)
- [MLIR Conversion Passes Header](https://mlir.llvm.org/doxygen/Conversion_2Passes_8h_source.html)
- [MLIR GPU Transforms Passes Header](https://mlir.llvm.org/doxygen/Dialect_2GPU_2Transforms_2Passes_8h.html)
- [GPU Kernel Outlining Source](https://mlir.llvm.org/doxygen/KernelOutlining_8cpp_source.html)
- [RankedTensorType Builder Reference](https://mlir.llvm.org/doxygen/classmlir_1_1RankedTensorType_1_1Builder.html)
- [MLIR Tensor Dialect](https://mlir.llvm.org/docs/Dialects/TensorOps/)
- [MLIR Arith Dialect](https://mlir.llvm.org/docs/Dialects/ArithOps/)
- [IREE / MLIR / Linalg Tutorial](https://iree.dev/community/blog/2024-01-29-iree-mlir-linalg-tutorial/)
