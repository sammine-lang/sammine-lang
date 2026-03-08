# Codegen Patterns

## Type Mapping (MLIR backend — `convertType()` in `MLIRGen.cpp`)

| TypeKind | MLIR Type |
|---|---|
| `I32_t`, `U32_t`, `Integer` | `builder.getI32Type()` |
| `I64_t`, `U64_t` | `builder.getI64Type()` |
| `F64_t`, `Flt` | `builder.getF64Type()` |
| `F32_t` | `builder.getF32Type()` |
| `Bool` | `getI1Type()` |
| `Char` | `getI8Type()` |
| `Unit` | `NoneType` (0 results) |
| `String` | `!llvm.ptr` (C string interop) |
| `Pointer` | `!llvm.ptr` |
| `Function` | `closureType` (`LLVMStructType{ ptr, ptr }`) |
| `Array` | `LLVMArrayType` |
| `Struct` | `structTypes` map |
| `Enum` | `enumTypes` map (or integer backing type) |
| `Tuple` | `LLVMStructType::getLiteral(ctx, elementTypes)` |

- `NumberExprAST`: integer → `stoll`, float → `stod` + `APFloat` (f32 uses `APFloat::convert(IEEEsingle())`)
- `CharExprAST`: `ConstantIntOp::create(builder, loc, uint8_t(value), 8)`
- Comparisons: integers/char/bool → `arith::CmpIOp`, float → `arith::CmpFOp`, pointer → `ICMP_EQ`/`ICMP_NE` only

## Assignment Codegen
`TokASSIGN` LHS forms (MLIR uses `LLVM::StoreOp`):
- `VariableExprAST` — look up alloca, store
- `DerefExprAST` — use operand (pointer), store
- `IndexExprAST` + `DerefExprAST` — `(*ptr)[i] = val`: GEP through pointer, store
- `IndexExprAST` + `VariableExprAST` — `arr[i] = val`: GEP into alloca, store

## Pointer-to-Array Indexing (`(*ptr)[i]`)
- Bounds checking via `emitBoundsCheck()`
- GEP through pointer into array element

## Enum Codegen
- **Layout**: `{ i32 tag, [N x i8] payload }` (named `sammine.enum.<name>`); unit-only enums → just `{ i32 }`
- **Type registration**: `generate()` pre-pass computes max payload size via DataLayout
- **Unit variant**: `UndefOp` + `InsertValueOp(tag, {0})`
- **Payload variant** (`emitEnumConstructor`): alloca enum type, store tag, GEP into byte buffer for payload fields, load complete value
- **Pattern matching** (`CaseExprAST`): cascading `cmpi` + `cf::CondBranchOp` + merge blocks with block arguments
- Payload extraction via byte-offset GEP, bindings stored in symbolTable

## While Loop Codegen
Header/body/exit blocks. `cf::CondBranchOp`/`cf::BranchOp`. Unit-typed (no result).

## First-Class Functions & Closures

### Closure Representation
- Fat pointer: `!llvm.struct<"sammine.closure", (ptr, ptr)>` (code_ptr, env_ptr)
- `getClosureFunctionType(FunctionType)` → `ret(ptr, params...)` (env ptr prepended)

### Wrapper Generation (`getOrCreateClosureWrapper`)
When a named function is used as a value (e.g. `let f = square`): generates `__wrap_<name>` that accepts-and-ignores env ptr, forwards args to original. Cached in `closure_wrappers`.

### Three Call Paths (`emitCallExpr`)
1. **Direct**: function found + not partial → `func::CallOp` (zero overhead)
2. **Partial**: found + `is_partial` → generate `__partial_N` wrapper with env struct for bound args → closure struct
3. **Indirect**: local variable → load closure, `ExtractValueOp` code/env → `LLVM::CallOp(funcType, codePtr, {envPtr, args...})`

### VariableExprAST Function References
If not in symbolTable but function exists + type is Function → create wrapper → build closure struct `{ wrapper, null }`

### Partial Application
- Bound args in stack-allocated env struct; `__partial_N` wrapper loads bound args via GEP+Load, calls original
- Unique names via `partial_counter`. **Stack limitation**: closures cannot escape defining scope

## Forward Declarations
`generate()` two phases:
1. Register types: all `StructDefAST`/`EnumDefAST` → MLIR types
2. Forward-declare: all `FuncDefAST`/`ExternAST`/`TypeClassInstanceAST` methods

Eliminates definition-ordering constraints. Typeclass methods use mangled names (e.g. `Sized<i32>::sizeof`).

## ASTProperties in Codegen
MLIR backend receives `const ASTProperties&` (stored as `props_` member). Node-specific decorated data is read from the side table, NOT from AST node fields:
- `props_.call(ast->id())->callee_func_type` — resolved function type for calls
- `props_.call(ast->id())->is_partial` — partial application detection
- `props_.call(ast->id())->resolved_name` — `optional<MonomorphizedName>` for monomorphized/typeclass call target; use `->mangled()` for name
- `props_.variable(ast->id())->is_enum_unit_variant` — enum unit variant detection
- `props_.binary(ast->id())->resolved_op_method` — `optional<MonomorphizedName>` for operator overload target; use `->mangled()` for name
- `ast->get_type()` — node type (reads from ASTProperties via static pointer)

## Operator Overloading (`resolved_op_method`)
Binary ops (`+`,`-`,`*`,`/`,`%`) check `props_.binary(ast->id())->resolved_op_method` — if `has_value()`, emit `func::CallOp` via `->mangled()` to typeclass method instead of native instruction.

## Typeclass Codegen
- Instance methods → regular functions with mangled names (e.g. `Add<i32>::add`). `TypeClassDeclAST`/`TypeClassInstanceAST` have no-op visitor stubs.
- Type checker does NOT mutate `ast->functionName` — codegen reads `resolved_name` from `CallProps` for generic/typeclass calls
- MLIR `emitCallExpr`: checks `cp->resolved_name` first (`->mangled()`), falls back to `ast->functionName.mangled()` for normal calls

## Export/IFunc Codegen
- Exported functions in library modules → IFunc so importers call via mangled name (`module__func`), C uses plain name
- Extern reuse declarations → IFunc mapping mangled name → original C symbol
- Mechanism: resolver function returning pointer to real function → `GlobalIFunc::create`
- MLIR: IFuncs created post-lowering in `Compiler::codegen_mlir()` on the LLVM IR module

## Runtime Function Declarations
`malloc`, `free`, `exit` declared in `generate()` as `llvm.func` or `func.func`.

## Generics / Monomorphization
- `emitFunction()`: if `is_generic()` → skip (template only)
- Monomorphized copies have empty `type_params` + concrete types → normal codegen
- `TypeKind::TypeParam` → abort in `convertType()` (must never reach codegen)
- Monomorphized defs injected at **front** of `DefinitionVec` — codegen'd before call sites
- Monomorphizer deep-clones AST with type substitution; must handle all ExprAST subtypes

## 4 Visitors to Update per New AST Node
1. `AstPrinterVisitor` (`src/ast/AstPrinterVisitor.cpp`) — visit + pre/post stubs
2. `BiTypeCheckerVisitor` (`src/typecheck/BiTypeChecker.cpp`) — synthesize + pre/post
3. `ScopeGeneratorVisitor` (`src/semantics/ScopeGeneratorVisitor.cpp`) — empty pre/post stubs
4. `GeneralSemanticsVisitor` (`include/semantics/GeneralSemanticsVisitor.h`) — empty pre/post stubs (inline)

Plus add `emitXxxExpr()` to `MLIRGenExpr.cpp` and `dynamic_cast` dispatch in `emitExpr()`.

## MLIR Backend

### Architecture & Files
- Direct recursive dispatch (`emitExpr` → `dynamic_cast`), NOT visitor pattern
- API: `mlirGen(MLIRContext&, ProgramAST*, moduleName, fileName, sourceText)` → `OwningOpRef<ModuleOp>`
- Lowering: `lowerMLIRToLLVMIR(ModuleOp, LLVMContext&)` → `unique_ptr<llvm::Module>`

| File | Contents |
|---|---|
| `MLIRGenImpl.h` | Class declaration, inline helpers (`llvmPtrTy()`, `llvmVoidTy()`), named constants |
| `MLIRGen.cpp` | `generate()`, `convertType()`, `emitVarDef()`, `emitBlock()`, `getTypeSize()`, `getOrCreateGlobalString()`, `emitAllocaOne()` |
| `MLIRGenFunction.cpp` | `emitFunction()`, `emitExtern()`, closures, partial application, `emitCallExpr()`, `emitFuncCallAndLLVMReturn()` |
| `MLIRGenExpr.cpp` | All expression emission (number, bool, char, string, binary, unary, if, while, array, pointer, struct, field, enum, case, tuple) |
| `MLIRLowering.cpp` | `lowerMLIRToLLVMIR()` pipeline |

### Named Constants (`MLIRGenImpl.h`)
`kClosureTypeName = "sammine.closure"`, `kStructTypePrefix = "sammine.struct."`, `kWrapperPrefix = "__wrap_"`, `kPartialPrefix = "__partial_"`, `kStringPrefix = ".str."`, `kMallocFunc = "malloc"`, `kFreeFunc = "free"`, `kExitFunc = "exit"`

### Dialect Mapping

| Construct | Dialect |
|---|---|
| Arithmetic, comparisons | `arith` |
| Function defs/calls/return | `func` |
| if/else, while, case/match | `cf` (ControlFlow) |
| Bounds checks | `scf` (scf.if + exit) |
| Arrays | `memref` |
| Closures, structs, enums, strings, pointers | `llvm` |

### Variable Model
- All non-array variables: `llvm.alloca` + `llvm.store` (uniform, mutable and immutable)
- Arrays: `LLVMArrayType` — stored as `!llvm.ptr` (alloca) in symbolTable; `emitVariableExpr` returns pointer without loading for arrays
- `emitVariableExpr`: `LLVMPointerType` → load via `LLVM::LoadOp`, array type → return directly, else SSA passthrough

### IfExprAST
- Uses `cf::CondBranchOp` + merge blocks with block arguments (NOT `scf.if`)
- Both-terminate case → merge block deleted. `scf.if` used only for bounds checks.

### Key Patterns
- `proto->get_type()` is full `FunctionType` — extract return via `std::get<FunctionType>(proto->get_type().type_data).get_return_type()`
- Uses `LexicalStack<mlir::Value>` for scoped variables
- Generic functions skipped (same as CgVisitor). Externs → `func.func` with `Private` visibility.

### Extracted Helpers
- `emitAllocaOne(elemType, loc)` — 1-element alloca: `ConstantIntOp(1, i64)` + `LLVM::AllocaOp`
- `emitPtrArrayGEP(ptr, idx, arrType, loc)` — GEP through pointer into array element
- `emitFuncCallAndLLVMReturn(callee, retType, args, loc)` — call + void-vs-value return (wrappers/partial)
- RAII: `mlir::OpBuilder::InsertionGuard` over manual save/restore; scope in `{}` block for early restore

### Tuple Codegen (MLIR)
- `emitTupleLiteralExpr()`: `UndefOp` + `InsertValueOp` for each element (same pattern as struct literals)
- `emitVarDef()` destructuring path: `ExtractValueOp` for each element index → alloca + store for each binding
- `convertType()`: `TypeKind::Tuple` → `LLVMStructType::getLiteral(ctx, elementTypes)` (anonymous struct)

### `getTypeSize()` Alignment
- Structs/Tuples: `llvm::alignTo()` for ABI padding (e.g. `{i32, f64}` = 16 bytes, not 12)
- Function = 16 (two pointers), Array = element_size * count, Enum = 4 (tag) + max payload

### Lowering Pipeline
```
scf-to-cf → arith-to-llvm → cf-to-llvm → func-to-llvm → reconcile-unrealized-casts → translateModuleToLLVMIR
```
Order matters: SCF → CF before CF → LLVM. (memref removed — arrays use LLVM dialect directly)

### CMake
- MLIR found via `-DMLIR_DIR=...`; Compiler.cpp loads 5 dialects: `arith`, `func`, `LLVM`, `scf`, `cf`
- `MLIRBackend` links: `MLIRIR`, `MLIRParser`, `MLIRSupport`, `MLIRPass`, dialect + conversion libraries

### Known Limitations
- `func.return` inside `scf.if` is invalid MLIR — early returns in if-branches not yet supported
- MLIR does not yet support module imports (IFunc creation happens at Compiler level on lowered LLVM IR)

## Build & Test
```bash
cmake --build build -j --target unit-tests e2e-tests
# MLIR backend requires -DMLIR_DIR when configuring:
cmake -B build -DSAMMINE_TEST=ON \
  -DLLVM_DIR=/path/to/llvm-project/build/lib/cmake/llvm \
  -DMLIR_DIR=/path/to/llvm-project/build/lib/cmake/mlir
./build/bin/sammine -f test.mn
```
