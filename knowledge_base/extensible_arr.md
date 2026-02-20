# Dynamic Heap Arrays via Fat Pointers (`ptr<[T]>`)

## Context

Currently sammine-lang has:
- `alloc(value)` → `ptr<T>` — heap-allocates a single value
- `[T;N]` — fixed-size stack arrays with bounds checking
- No way to heap-allocate dynamically-sized arrays

The goal is to add `ptr<[T]>` (no size parameter) as a **fat pointer** type — a `{data_ptr, length}` pair that carries its length at runtime, enabling bounds-checked indexing on heap arrays.

### Syntax (driven by type annotation)

```
let p : ptr<[i32]> = alloc(32);       # 32 zero-initialized i32s
let p : ptr<[i32]> = alloc(n);        # n zero-initialized i32s (runtime size)
let p : ptr<[i32]> = alloc([1,2,3]);  # 3 i32s from literal
p[1] = 42;                             # bounds-checked index assignment
printf("%d\n", p[2]);                  # bounds-checked index read
free(p);                               # frees the heap array
```

### Type distinction

| Type | Representation | Bounds checking |
|------|---------------|-----------------|
| `ptr<i32>` | thin pointer (opaque `ptr`) | none |
| `ptr<[i32;3]>` | thin pointer (opaque `ptr`) | compile-time size known |
| `ptr<[i32]>` | **fat pointer** (`{ ptr, i64 }`) | runtime length stored |
| `[i32;3]` | stack `[3 x i32]` | compile-time size known |

---

## Phase 1: Parser — Allow `[T]` without size

**File: `src/Parser.cpp` — `ParseTypeExpr()` (~line 391)**

Currently `[T;SIZE]` requires both element type and integer size. Modify to also accept `[T]` (no semicolon, no size):

- After parsing element type, check for `;` vs `]`
- If `;` → parse size as before → `ArrayTypeExprAST(element, size)`
- If `]` directly → dynamic array → `ArrayTypeExprAST(element, std::nullopt)`

**File: `include/ast/Ast.h` — `ArrayTypeExprAST` (~line 51)**

Change `size_t size` to `std::optional<size_t> size`. Update `to_string()` to handle both forms.

**Build & test after this phase.**

---

## Phase 2: Type system — Dynamic array support

**File: `include/typecheck/Types.h` — `ArrayType` (~line 59)**

- Change `size_t size` → `std::optional<size_t> size`
- Add `bool is_dynamic() const { return !size.has_value(); }`
- Update `get_size()` to assert non-dynamic or return optional
- Update `operator==` and `operator<`

**File: `include/typecheck/Types.h` — `Type` struct**

- Add factory: `static Type DynArray(Type element)` → `Type{TypeKind::Array, ArrayType(element, std::nullopt)}`
- Keep existing `Type::Array(Type element, size_t size)` for fixed-size

**File: `src/typecheck/BiTypeChecker.cpp` — `resolve_type_expr()` or equivalent**

- When resolving `ArrayTypeExprAST` with no size → produce `Type::DynArray(element_type)`
- When resolving with size → produce `Type::Array(element_type, size)` (existing)

**No new TypeKind needed** — both fixed and dynamic arrays use `TypeKind::Array`. The `is_dynamic()` method distinguishes them.

**Build & test after this phase.**

---

## Phase 3: TypeConverter — Fat pointer LLVM type

**File: `src/codegen/TypeConverter.cpp`**

Update `get_type()` for `TypeKind::Pointer`:

```cpp
case TypeKind::Pointer: {
  auto pointee = std::get<PointerType>(t.type_data).get_pointee();
  if (pointee.type_kind == TypeKind::Array) {
    auto &arr = std::get<ArrayType>(pointee.type_data);
    if (arr.is_dynamic()) {
      // Fat pointer: { ptr, i64 }
      return llvm::StructType::get(context, {
        llvm::PointerType::get(context, 0),
        llvm::Type::getInt64Ty(context)
      });
    }
  }
  // Thin pointer (existing)
  return llvm::PointerType::get(context, 0);
}
```

Consider adding a helper: `TypeConverter::is_fat_pointer(Type t)` and `TypeConverter::get_fat_ptr_type()`.

**Build & test after this phase** (existing tests should still pass — no existing code creates dynamic arrays).

---

## Phase 4: Type checker — Bidirectional alloc checking

**File: `src/typecheck/BiTypeChecker.cpp`**

### 4a: Update `visit(VarDefAST*)` (~line 48)

Add special handling when RHS is `AllocExprAST` and declared type is `ptr<[T]>` (dynamic):

```
- Synthesize declared type from annotation
- If declared type is ptr<[T]> where arr is dynamic:
  - If alloc operand is ArrayLiteralExprAST:
    → synthesize literal, check element types match T
    → set alloc type to ptr<[T]> with dynamic length
  - Else (operand is count expression):
    → synthesize operand, verify it's integer (i32 or i64)
    → set alloc type to ptr<[T]> with dynamic length
- Else: existing behavior (alloc(value) → ptr<typeof(value)>)
```

### 4b: Update `synthesize(AllocExprAST*)` (~line 534)

Keep existing behavior for the synthesis direction. The check direction is handled in VarDefAST.

### 4c: Update `synthesize(IndexExprAST*)` (~line 578)

Currently only allows `TypeKind::Array`. Add support for `ptr<[T]>`:

```
- If array_expr type is ptr<[T]> (pointer to dynamic array):
  → validate index is integer
  → return element type T
- If array_expr type is [T;N] (existing):
  → existing behavior with static bounds check
```

### 4d: Update `synthesize(LenExprAST*)` (~line 612)

Currently only allows `TypeKind::Array`. Add support for `ptr<[T]>`:
- Extract element type from the pointee array type
- Return `Type::I64_t()` for dynamic arrays (length is i64)

### 4e: Update `synthesize(FreeExprAST*)` (~line 542)

Should already work — `ptr<[T]>` has `TypeKind::Pointer`. No change needed.

**Build & test after this phase.**

---

## Phase 5: Codegen — Fat pointer operations

**File: `src/codegen/CodegenVisitor.cpp`**

### 5a: Alloc codegen — `postorder_walk(AllocExprAST*)` (~line 400)

Add a branch for when `ast->type` is `ptr<[T]>` (fat pointer):

**Count-based allocation:**
```
1. Get count value from operand (i32/i64)
2. Get element LLVM type from T
3. Compute: size = sizeof(T) * count
4. Call malloc(size)
5. Call memset(ptr, 0, size) — zero-initialize
6. Build fat pointer struct: { malloc_result, count }
7. ast->val = fat_ptr_struct
```

**Array-literal-based allocation:**
```
1. Get elements from the literal
2. count = number of elements
3. Call malloc(sizeof(T) * count)
4. Store each element value into the allocated memory via GEP
5. Build fat pointer struct: { malloc_result, count }
6. ast->val = fat_ptr_struct
```

Need helper to declare `memset` in `CodegenUtils` (alongside malloc/free).

### 5b: Index codegen — `visit(IndexExprAST*)` + `postorder_walk(IndexExprAST*)` (~line 476)

Currently hardcoded for stack arrays (looks up alloca by variable name).

Add branch for `ptr<[T]>`:
```
1. Visit array_expr normally (get the fat pointer value)
2. Extract data pointer: CreateExtractValue(fat_ptr, 0)
3. Extract length: CreateExtractValue(fat_ptr, 1)
4. Visit index_expr to get index value
5. Emit bounds check: index >= 0 && index < length
6. GEP into data pointer: CreateGEP(element_type, data_ptr, {index})
7. Load element: CreateLoad(element_type, gep)
```

Note: single-index GEP for pointer arithmetic (not `{0, idx}` form used for arrays).

### 5c: Index assignment — `postorder_walk(BinaryExprAST*)` (~line 144)

Currently handles `arr[idx] = value`. Add branch for `ptr<[T]>`:
```
1. Detect LHS is IndexExprAST on a ptr<[T]>
2. Get fat pointer, extract data_ptr and length
3. Bounds check
4. GEP into data_ptr
5. Store RHS value
```

### 5d: Free codegen — `postorder_walk(FreeExprAST*)` (~line 420)

Currently calls `free(ptr_val)`. For fat pointer:
```
1. Extract data pointer: CreateExtractValue(fat_ptr, 0)
2. Call free(data_ptr)
```

### 5e: Len codegen — `postorder_walk(LenExprAST*)` (~line 505)

Currently returns compile-time constant for fixed arrays. For fat pointer:
```
1. Extract length: CreateExtractValue(fat_ptr, 1)
2. ast->val = length
```

### 5f: Variable store/load

`VarDefAST` codegen needs to handle fat pointer structs — the alloca must be of struct type `{ ptr, i64 }`, and store/load the whole struct.

**Build & test after this phase.**

---

## Phase 6: E2E tests

**New test files in `e2e-tests/compilables/ptr/`:**

1. `heap_arr_basic.mn` — alloc with count, index, assign, free
2. `heap_arr_literal.mn` — alloc with array literal
3. `heap_arr_runtime_size.mn` — alloc with variable count
4. `heap_arr_len.mn` — len() on heap array
5. `heap_arr_f64.mn` — works with other element types
6. `heap_arr_oob.mn` — bounds check triggers on out-of-bounds

**Error test in `e2e-tests/errors/`:**

7. `heap_arr_count_type_error.mn` — non-integer count produces type error

---

## Files to modify (summary)

| File | Changes |
|------|---------|
| `include/ast/Ast.h` | `ArrayTypeExprAST`: size → `optional<size_t>` |
| `include/typecheck/Types.h` | `ArrayType`: size → `optional<size_t>`, add `is_dynamic()`, add `Type::DynArray()` |
| `src/Parser.cpp` | `ParseTypeExpr()`: allow `[T]` without size |
| `src/typecheck/BiTypeChecker.cpp` | `visit(VarDefAST*)`: bidirectional alloc checking; `synthesize(IndexExprAST*)`: ptr<[T]> support; `synthesize(LenExprAST*)`: ptr<[T]> support |
| `src/codegen/TypeConverter.cpp` | `get_type()`: fat pointer struct for `ptr<[T]>` |
| `src/codegen/CodegenVisitor.cpp` | Alloc, index, index-assign, free, len for fat pointers |
| `src/codegen/CodegenUtils.cpp` | Declare `memset` |
| `src/ast/AstPrinterVisitor.cpp` | Update printer for dynamic array types |
| `e2e-tests/` | New test files |

## Future: Custom Allocators

The current design hardcodes `malloc`/`free`. The fat pointer representation `{ptr, len}` is allocator-agnostic — it doesn't care where the memory came from.

When traits/interfaces are added to the language, `alloc`/`free` can be extended to accept an allocator:

```
# Option A: explicit allocator argument (no traits needed)
let arena = Arena::new(4096);
let p : ptr<[i32]> = alloc(32, arena);
free(p, arena);

# Option B: allocator trait (requires trait system)
trait Allocator {
  let alloc(size: i64) -> ptr<i32>;
  let free(p: ptr<i32>) -> ();
}
```

No changes to the fat pointer type or indexing/bounds-checking logic are needed — only the allocation/deallocation backend changes. This is an orthogonal concern to be addressed when the trait system lands.

---

## Verification

After each phase, run:
```bash
cmake --build build -j --target unit-tests e2e-tests
```
All existing 60 tests must continue passing. New tests added in Phase 6.
