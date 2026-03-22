# Plan: Fix immutable array global constant UB

## Bug Summary

When an immutable array with all-constant elements is defined (e.g. `let a: [3]i32 = [10, 20, 30]`), the MLIR codegen emits it as a **read-only global constant** (`isConstant=true`) and stores the global's address in the symbol table. If a user takes `&a` and passes it to a function that writes through the pointer (e.g. `write_elem(&a, 1, 99)`), the write targets read-only memory — undefined behavior.

At O2, LLVM constant-folds everything and the bug is invisible. At O0/O1, the write silently fails and reads back stale data.

## Root Cause

`src/codegen/MLIRGen.cpp` lines 938-953, in `emitVarDefArray()`:

```cpp
// For immutable arrays with all-constant elements, emit as global constant
if (!ast->is_mutable) {
    if (auto *arrLit = ...) {
      if (allConst) {
        auto globalPtr = emitGlobalConstArray(arrLit, ast->get_type(), location);
        symbolTable.registerNameT(ast->TypedVar->name, globalPtr);
        return globalPtr;
      }
    }
}
```

`emitGlobalConstArray()` creates a `GlobalOp` with `isConstant=true`. The symbol table entry for `a` points directly to this global. Then `emitAddrOfExpr(&a)` → `emitLValue(a)` → `symbolTable.get_from_name("a")` returns the global constant pointer.

## Reproducer

```sammine
import std;

let write_elem(arr: ptr<[3]i32>, i: i32, val: i32) -> i32 {
  (*arr)[i] = val;
  (*arr)[i]
}

let main() -> i32 {
  let a : [3]i32 = [10, 20, 30];
  std::printf("%d\n", write_elem(&a, 1, 99));  // Expected: 99, got: 20 at O0/O1
  return 0;
}
```

## Fix Options

### Option A: Always alloca for arrays (simplest, safe)
Remove the global constant optimization entirely — always alloca + store like mutable arrays. Simple, but loses the optimization for truly read-only arrays.

### Option B: Copy-on-address-of (ideal, harder)
Keep the global constant optimization, but when `&a` is emitted for an immutable array backed by a global constant, emit an alloca, memcpy from the global, and return the alloca address. This preserves the read-only optimization for arrays whose address is never taken.

### Option C: Emit global as non-constant (compromise)
Change `emitGlobalConstArray` to use `isConstant=false`. The data lives in writable memory (.data instead of .rodata). Simple change but loses the read-only memory protection.

## Recommended Fix

**Option A** for now — it's the simplest and most correct. The optimization can be re-added later with Option B when we have a "address taken" analysis.

The change: in `emitVarDefArray()`, remove the early return for all-const immutable arrays, so they fall through to the normal alloca path (lines 969-971). The alloca path already handles this correctly via `emitArrayLiteralExpr` which creates the global and then loads from it into a stack alloca.

Actually, looking more carefully at the code flow: the alloca path at lines 969-971 stores `initVal` (the result of `emitExpr` on the array literal) in the symbol table. For array literals, `emitExpr` calls `emitArrayLiteralExpr` which already creates a global constant and returns an `AddressOfOp` pointing to it. So falling through would still register the global pointer.

The real fix is: for immutable arrays, do what mutable arrays do — alloca + load from global + store to alloca. Or simply remove the `!ast->is_mutable` early return and let all arrays go through the mutable path.

## Optimal Prompt

```
Fix a codegen bug where immutable constant arrays are backed by read-only global
memory, causing UB when their address is taken and written through.

The bug is in src/codegen/MLIRGen.cpp, function emitVarDefArray(), lines 938-953.
When an immutable array has all-constant elements, it emits a GlobalOp with
isConstant=true and registers the global pointer in the symbol table. Taking &a
then returns this read-only pointer, so writes through it are UB.

Read knowledge_base/plans/fix_immutable_array_global_const.md for the full analysis.

The fix: make immutable arrays also go through the alloca + copy path (same as
mutable arrays at lines 959-968). Remove the early return at lines 948-953 so
immutable const arrays get an alloca with the global's data copied in, ensuring
&a returns a writable stack address.

Test with: sammine --jit -f e2e-tests/compilables/ptr/ptr_array_index.mn
Expected output: 10, 30, 99
Then run full e2e tests to confirm no regressions.
```
