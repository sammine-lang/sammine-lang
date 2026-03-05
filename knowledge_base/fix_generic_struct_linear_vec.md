# Fixes: Generic Struct Returns, Linear Branch Checking, Vec<T>

This documents the chain of compiler bugs discovered and fixed while implementing `Vec<T>` in the stdlib. Each fix is described with root cause, symptoms, and the change made.

---

## 1. Generic Functions Returning Generic Structs → Poisoned Type

**Symptom:** Any generic function whose return type is a generic struct (e.g., `make_box<T>(x: T) -> Box<T>`) produces a `Poisoned` return type. Codegen crashes with `map::at: key not found` looking up `"Box<T>"` in the struct type map.

**Root cause:** `resolve_type_expr` in `BiTypeChecker.h` (around line 426) for `GenericTypeExprAST` with unresolved type params (e.g., `Box<T>` where `T` is a TypeParam) returned `Type::Poisoned()`. This poisoned the function type stored in `generic_func_defs`. When `synthesize_generic_call` later tried `substitute(func.get_return_type(), bindings)`, substituting Poisoned yields Poisoned.

**Why it was hidden:** Generic function bodies are NOT type-checked (skipped at `BiTypeChecker.cpp:106-110`). Only the prototype is visited. Existing tests only tested generic functions with primitive or TypeParam return types — never `Box<T>`.

**Fix (two parts):**

### Part A: Placeholder StructType in `resolve_type_expr` (`BiTypeChecker.h`)

When `has_unresolved_type_param` is true and the type is a known struct, build a placeholder `StructType` with TypeParam fields instead of returning Poisoned:

```cpp
if (is_struct) {
    auto *sdef = struct_it->second;
    // register bindings (e.g. T → TypeParam("T"))
    for (auto &[pname, ptype] : bindings)
        typename_to_type.registerNameT(pname, ptype);
    // resolve each field type (may be TypeParam)
    for (auto &member : sdef->struct_members) {
        fnames.push_back(member->name);
        ftypes.push_back(resolve_type_expr(member->type_expr.get()));
    }
    auto placeholder = Type::Struct(
        QualifiedName::from_parts({mangled}), fnames, ftypes);
    typename_to_type.registerNameT(mangled, placeholder);
    return placeholder;
}
```

This gives `substitute()` and parameter type checking something real to work with — a struct whose fields contain TypeParam types that can be substituted.

### Part B: Re-resolve from AST in `synthesize_generic_call` (`BiTypeCheckerSynthesize.cpp`)

Instead of relying solely on `substitute()` (which can't reconstruct the concrete struct name), register concrete bindings in a **temporary scope** and call `resolve_type_expr` on the prototype's param/return type ASTs:

```cpp
// Register concrete bindings in a temporary scope
typename_to_type.push_context();
for (auto &[pname, ptype] : bindings)
    typename_to_type.registerNameT(pname, ptype);

// Resolve param types from AST (triggers proper struct instantiation)
auto *param_type_ast = generic_def->Prototype->parameterVectors[i]->type_expr.get();
auto expected = param_type_ast ? resolve_type_expr(param_type_ast)
                               : substitute(params[i], bindings);

// ... check args ...
typename_to_type.pop_context();
```

Same pattern for return type. The temporary scope prevents type param names (like `T`) from leaking into the outer scope (which caused `type_param_shadows_struct.mn` to regress).

**Key insight:** `resolve_type_expr` with concrete bindings follows the normal struct instantiation path — creating `Vec<i32>` with proper name, fields, and codegen registration. `substitute()` alone can't do this because it doesn't trigger instantiation.

**Files changed:**
- `include/typecheck/BiTypeChecker.h` — `resolve_type_expr` placeholder
- `src/typecheck/BiTypeCheckerSynthesize.cpp` — `synthesize_generic_call` re-resolve + `substitute()` StructType case

---

## 2. Monomorphizer Losing `is_linear` on Pointer Types

**Symptom:** `alloc<T>(cap)` returns `'ptr<T>` (linear), but after monomorphization, the struct field expected `ptr<T>` (non-linear). Error: `Field 'data': expected type ptr<i32>, got 'ptr<i32>`.

**Root cause:** `Monomorphizer::clone_type_expr` for `PointerTypeExprAST` was not passing the `is_linear` flag to the cloned node.

**Fix:** `src/typecheck/Monomorphizer.cpp` — added `ptr->is_linear` as second arg:

```cpp
return std::make_unique<PointerTypeExprAST>(
    clone_type_expr(ptr->pointee.get()), ptr->is_linear);
```

---

## 3. Linear Checker: Branch Consistency for Child Vars

**Symptom:** In `push<T>`, an if/else where the then branch passes `v.data` to a function and the else branch moves `v.data` into a struct literal — the linear checker reports `v.data must be consumed before scope exit`, even though both branches consume it.

**Root cause:** `check_branch_consistency` only checked top-level var states in the snapshot, not child var states. `v` is a wrapper struct; its own state stays `Unconsumed` — only `v.data` (a child in `v.children["data"]`) gets consumed. The function never looked at children.

**Fix:** `src/typecheck/LinearTypeChecker.cpp` — added a nested loop in `check_branch_consistency` that iterates `before_info.children` for each top-level var, checking child consumption consistency across branches:

```cpp
for (auto &[child_name, child_before] : before_info.children) {
    if (child_before.state != VarState::Unconsumed) continue;
    // Check each branch's snapshot for this child's state
    // Apply same consistency logic (all consume or none)
}
```

---

## 4. Linear Checker Running on Generic Templates

**Symptom:** Monomorphized generic functions with inner generic calls (e.g., `push<i32>` calling `grow_buf<i32>`) fail linear checking because the inner call has no `callee_func_type` in CallProps.

**Root cause:** The linear checker iterated ALL `DefinitionVec` entries, including **original generic templates**. Generic templates have AST node IDs that were never processed by the type checker (only the monomorphized copies were type-checked). So `props_->call(ast->id())` returned nullptr for calls inside generic templates, causing the checker to fall back to "just walk args" without consuming linear vars.

**How we found it:** Debug prints showed the type checker setting CallProps for node id=175 (monomorphized call), but the linear checker looking for id=26 (original template call).

**Fix:** `src/typecheck/LinearTypeChecker.cpp` — skip generic templates at the top of `check_func`:

```cpp
void LinearTypeChecker::check_func(FuncDefAST *ast) {
    if (ast->Prototype->is_generic())
        return;
    // ... rest of checking
}
```

Generic functions are checked via their monomorphized copies, which have proper CallProps and concrete types.

---

## 5. FieldAccessExprAST in check_struct_literal / check_array_literal / check_tuple_literal

**Symptom:** `Vec<T> { data: v.data, ... }` didn't consume `v.data` as a linear child.

**Root cause:** The linear checker's `check_struct_literal`, `check_array_literal`, and `check_tuple_literal` only handled `VariableExprAST` field values. `FieldAccessExprAST` (e.g., `v.data`) was not handled — it fell through to `check_stmt` which doesn't consume.

**Fix:** `src/typecheck/LinearTypeChecker.cpp` — added `FieldAccessExprAST` handling after the existing `VariableExprAST` check in all three functions:

```cpp
} else if (auto *fa = llvm::dyn_cast<FieldAccessExprAST>(field_val.get())) {
    if (auto *obj = llvm::dyn_cast<VariableExprAST>(fa->object_expr.get())) {
        auto *child = find_child(obj->variableName, fa->field_name);
        if (child) {
            consume(child, ast->get_location(), "moved into struct");
            continue;
        }
    }
}
```

---

## Vec<T> Implementation Notes

The stdlib `Vec<T>` (`stdlib/vec.mn`) uses these features:

- **Linear ownership:** `data: 'ptr<T>` — Vec must be freed via `vec::delete`
- **Functional style:** `let v1 = vec::push(v0, val)` — no mutation, returns new Vec
- **Growth:** `grow_buf<T>` does element-by-element copy (alloc new → while loop copy → free old)
- **`realloc` not used** because C's `realloc` takes `void*` but sammine-lang has strict pointer typing (`'ptr<i32>` ≠ `'ptr<char>`)
- **`len` is a keyword** — struct field is named `length`
- **No variable shadowing** — must use `v0`, `v1`, `v2`, etc.
- **`let mut` needed** for index writes (`new_d[i] = old_d[i]` requires `let mut new_d`)
- Generic stdlib code goes in `STDLIB_COPIED` (not `STDLIB_COMPILED`) because it can't be pre-compiled
