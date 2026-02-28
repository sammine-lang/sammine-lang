# Codegen Patterns

## TypeConverter (`src/codegen/TypeConverter.cpp`)

| TypeKind | LLVM Type | MLIR Type |
|---|---|---|
| `I32_t` | `getInt32Ty` | `builder.getI32Type()` |
| `I64_t` | `getInt64Ty` | `builder.getI64Type()` |
| `F64_t` | `getDoubleTy` | `builder.getF64Type()` |
| `Bool` | `getInt1Ty` | `getI1Type()` |
| `Char` | `getInt8Ty` | `getI8Type()` |
| `Unit` | `getVoidTy` | `NoneType` (0 results) |
| `String` | `StructType{ptr, i32}` | `!llvm.ptr` (C string interop) |
| `Pointer` | `PointerType::get(ctx, 0)` (opaque) | `!llvm.ptr` |
| `Function` | `sammine.closure` struct | `closureType` (`LLVMStructType{ ptr, ptr }`) |
| `Array` | N/A (alloca-based) | `MemRefType::get({size}, elemType)` |
| `Struct` | `get_struct_type(name)` | `structTypes` map |
| `Enum` | `get_enum_type(name)` | `enumTypes` map |
| `Tuple` | N/A | `LLVMStructType::getLiteral(ctx, elementTypes)` |

- `NumberExprAST`: `I32_t`→`stoi`, `I64_t`→`stoll`, `F64_t`→`stod` (suffix already stripped by type checker)
- `CharExprAST`: LLVM `ConstantInt::get(getInt8Ty, value)`, MLIR `ConstantIntOp::create(builder, loc, uint8_t(value), 8)`
- `get_cmp_func(Type, Type, TokenType)` → `CmpInst::Predicate`: integers/char/bool→`ICMP_*`, float→`FCMP_*`, pointer→`ICMP_EQ`/`ICMP_NE` only, unit/function→abort

## CgVisitor (`src/codegen/CodegenVisitor.cpp`)
- Extends `ScopedASTVisitor`; `allocaValues` stack maps variable names → `AllocaInst*`
- **Deref (`*p`)**: visit traverses operand, postorder does `CreateLoad(pointee_type, val)`
- **AddrOf (`&x`)**: visit skips operand load — gets alloca directly from `allocaValues`
- **Alloc (`alloc<T>(count)`)**: `sizeof(T)*count` via DataLayout → `malloc(total_size)` → pointer
- **Free (`free(expr)`)**: calls `free(operand_val)`, sets `ast->val = nullptr`

## Assignment Codegen (`postorder_walk(BinaryExprAST*)`)
`TokASSIGN` LHS forms (both backends — MLIR uses `LLVM::StoreOp`/`memref::StoreOp`):
- `VariableExprAST` — look up alloca, `CreateStore(RHS, alloca)`
- `DerefExprAST` — use `operand->val` (pointer), `CreateStore(RHS, pointer)`
- `IndexExprAST` + `DerefExprAST` — `(*ptr)[i] = val`: GEP through pointer, store
- `IndexExprAST` + `VariableExprAST` — `arr[i] = val`: GEP into alloca, store

## Pointer-to-Array Indexing (`(*ptr)[i]`)
- `visit(IndexExprAST*)`: if array_expr is `DerefExprAST`, visits only `deref->operand` (skips full array load)
- `postorder_walk`: `GEP(arr_type, ptr_val, {0, idx})` + `CreateLoad` for element access
- Bounds checking via `emitBoundsCheck()` in both direct and pointer-to-array cases
- Immutable pointer params get `readonly nocapture` attributes (`FunctionCodegen.cpp`)

## Array Equality (`emitArrayComparison`)
- Element-wise loop comparison, short-circuits on first mismatch → `false`; all match → `true`
- `!=` via `CreateNot` of `==` result; nested arrays handled recursively
- Dispatched from `postorder_walk(BinaryExprAST*)` when `lhs_type.type_kind == TypeKind::Array`

## Enum Codegen
- **Layout**: `{ i32 tag, [N x i8] payload }` (named `sammine.enum.<name>`); unit-only enums → just `{ i32 }`
- **Type registration**: `preorder_walk(ProgramAST*)`/`generate()` pre-pass computes max payload size via DataLayout
- **Unit variant**: `UndefValue` + `InsertValue(tag, {0})`
- **Payload variant** (`emitEnumConstructor`): alloca enum type, store tag via `StructGEP(0)`, GEP into byte buffer for payload fields, load complete value
- **Pattern matching** (`CaseExprAST`): LLVM uses `switch` on tag + PHI merge; MLIR uses cascading `cmpi` + `cf::CondBranchOp` + merge blocks with block arguments
- Both backends: payload extraction via byte-offset GEP, bindings stored in allocaValues/symbolTable

## While Loop Codegen
Both backends: header/body/exit blocks. LLVM: `CreateCondBr`/`CreateBr`. MLIR: `cf::CondBranchOp`/`cf::BranchOp`. Unit-typed (no result).

## First-Class Functions & Closures

### Closure Representation
- Fat pointer: `%sammine.closure = type { ptr, ptr }` (code_ptr, env_ptr)
- Created once in `preorder_walk(ProgramAST*)`; `get_type(Function)` → `%sammine.closure`
- `get_closure_function_type(FunctionType)` → `ret(ptr, params...)` (env ptr prepended)

### Wrapper Generation (`getOrCreateClosureWrapper`)
When a named function is used as a value (e.g. `let f = square`): generates `__wrap_<name>` that accepts-and-ignores env ptr, forwards args to original. Cached in `closure_wrappers`.

### Three Call Paths (`postorder_walk(CallExprAST*)`)
1. **Direct**: `Module->getFunction` found + not partial → `CreateCall(callee, args)` (zero overhead)
2. **Partial**: found + `is_partial` → generate `__partial_N` wrapper with env struct for bound args → `%sammine.closure`
3. **Indirect**: callee not in Module (local variable) → load closure, `ExtractValue` code/env → `CreateCall(funcType, codePtr, {envPtr, args...})`

### VariableExprAST Function References
If not in allocaValues but `Module->getFunction` found + type is Function → create wrapper → build `%sammine.closure { wrapper, null }`

### Partial Application
- Bound args in stack-allocated env struct; `__partial_N` wrapper loads bound args via GEP+Load, calls original
- Unique names via `partial_counter`. **Stack limitation**: closures cannot escape defining scope

## Forward Declarations
`preorder_walk(ProgramAST*)` two phases:
1. Register types: all `StructDefAST`/`EnumDefAST` → LLVM types
2. Forward-declare: all `FuncDefAST`/`ExternAST`/`TypeClassInstanceAST` methods → `forward_declare()`/`getOrInsertFunction()`

Eliminates definition-ordering constraints. Typeclass methods use mangled names (e.g. `Sized__i32__sizeof`).

## Operator Overloading (`resolved_op_method`)
Binary ops (`+`,`-`,`*`,`/`,`%`) check `ast->resolved_op_method` — if set, emit function call to typeclass method instead of native instruction. Both backends (MLIR uses `func::CallOp`).

## Typeclass Codegen
- Instance methods → regular functions with mangled names. `TypeClassDeclAST`/`TypeClassInstanceAST` have no-op visitor stubs.
- LLVM: type checker rewrites `functionName` to mangled name pre-codegen → uses `functionName.mangled()` directly
- MLIR: `emitCallExpr` checks `resolved_generic_name` first, falls back to `functionName.mangled()`

## Export/IFunc Codegen
- Exported functions in library modules → IFunc so importers call via mangled name (`module__func`), C uses plain name
- Extern reuse declarations → IFunc mapping mangled name → original C symbol
- Mechanism: resolver function returning pointer to real function → `GlobalIFunc::create`
- MLIR: IFuncs created post-lowering in `Compiler::codegen_mlir()` on the LLVM IR module

## Runtime Function Declarations
`malloc`, `free`, `printf`, `exit` declared in `preorder_walk(ProgramAST*)`. Helpers: `CodegenUtils::declare_malloc()`/`declare_free()`/`declare_fn()` in `src/codegen/CodegenUtils.cpp`.

## Generics / Monomorphization
- `CgVisitor::visit(FuncDefAST*)`: if `is_generic()` → skip (template only)
- Monomorphized copies have empty `type_params` + concrete types → normal codegen
- `TypeKind::TypeParam` → abort in `get_type()`/`get_cmp_func()` (must never reach codegen)
- Monomorphized defs injected at **front** of `DefinitionVec` — codegen'd before call sites
- Monomorphizer deep-clones AST with type substitution; must handle all ExprAST subtypes

## 5 Visitors to Update per New AST Node
1. `AstPrinterVisitor` (`src/ast/AstPrinterVisitor.cpp`) — visit + pre/post stubs
2. `BiTypeCheckerVisitor` (`src/typecheck/BiTypeChecker.cpp`) — synthesize + pre/post
3. `CgVisitor` (`src/codegen/CodegenVisitor.cpp`) — codegen in pre/post/visit
4. `ScopeGeneratorVisitor` (`src/semantics/ScopeGeneratorVisitor.cpp`) — empty pre/post stubs
5. `GeneralSemanticsVisitor` (`include/semantics/GeneralSemanticsVisitor.h`) — empty pre/post stubs (inline)

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
- Arrays: `memref<NxT>` registered directly in `symbolTable`
- `emitVariableExpr`: `MemRefType` → array (return directly), `LLVMPointerType` → load via `LLVM::LoadOp`, else SSA passthrough

### IfExprAST
- Uses `cf::CondBranchOp` + merge blocks with block arguments (NOT `scf.if`)
- Both-terminate case → merge block deleted. `scf.if` used only for bounds checks.

### Key Patterns
- `proto->type` is full `FunctionType` — extract return via `std::get<FunctionType>(proto->type.type_data).get_return_type()`
- Uses `LexicalStack<mlir::Value, std::monostate>` for scoped variables
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
scf-to-cf → arith-to-llvm → memref-to-llvm → cf-to-llvm → func-to-llvm → reconcile-unrealized-casts → translateModuleToLLVMIR
```
Order matters: SCF → CF before CF → LLVM; MemRef before final reconcile.

### CMake
- MLIR found via `-DMLIR_DIR=...`; Compiler.cpp loads 6 dialects: `arith`, `func`, `LLVM`, `scf`, `cf`, `memref`
- `MLIRBackend` links: `MLIRIR`, `MLIRParser`, `MLIRSupport`, `MLIRPass`, dialect + conversion libraries

### Known Limitations
- `func.return` inside `scf.if` is invalid MLIR — early returns in if-branches not yet supported
- MLIR does not yet support module imports (IFunc creation happens at Compiler level on lowered LLVM IR)

### Coverage
Both backends support the full language (except limitations above).

## Build & Test
```bash
cmake --build build -j --target unit-tests e2e-tests
# MLIR backend requires -DMLIR_DIR when configuring:
cmake -B build -DSAMMINE_TEST=ON \
  -DLLVM_DIR=/path/to/llvm-project/build/lib/cmake/llvm \
  -DMLIR_DIR=/path/to/llvm-project/build/lib/cmake/mlir
./build/bin/sammine -f test.mn
```
