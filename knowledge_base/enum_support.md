# Enum Support

## Syntax

| Construct | Example |
|---|---|
| Definition | `enum Color = Red \| Green \| Blue;` |
| Payload variants | `enum Shape = Circle(f64) \| Rect(f64, f64) \| Point;` |
| Integer-backed | `enum Status = OK(0) \| Err(1);` |
| Explicit backing type | `enum Flags: u32 = Read(1) \| Write(2) \| Exec(4);` |
| Generic enum | `enum Option<T> = Some(T) \| None;` |
| Qualified construction | `Color::Red`, `Shape::Circle(3.14)` |
| Unqualified construction | `Red`, `Some(42)` — rewritten to qualified form by scope generator |
| Case expression | `case expr { Color::Red => body, _ => default, }` |
| Bitwise on int-backed | `Read \| Write` — `&`, `\|`, `^`, `<<`, `>>` supported |

## Variant Access

- **Qualified** (`Color::Red`) — always works
- **Unqualified** (`Red`) — scope generator rewrites to qualified form before type checking
- Scope generator populates `variant_to_enum` map (variant name → enum name) during `preorder_walk(ProgramAST*)`
- `postorder_walk(CallExprAST*)` rewrites unqualified variant calls → qualified form
- **Invariant**: the type checker assumes all variant calls arrive pre-qualified; it does NOT resolve unqualified names
- Both unit and payload variants parse as `CallExprAST` with a qualified name (after rewriting)
- Enums are NOT transitive through imports — file B must directly import file A

## LLVM Representation

| Layout | Description |
|---|---|
| Tagged union | `{ i32 tag, [N x i8] payload }` — N = max payload size across variants |
| Named type | `sammine.enum.<Name>` |
| Unit-only enum | `{ i32 }` (no payload buffer) |
| Construction | alloca + GEP + store for tag and payload |
| Pattern extraction | alloca + GEP + load from byte buffer at correct offsets |

## Type System

- `TypeKind::Enum` with `EnumType` class using `QualifiedName` (nominal equality by mangled name). Has `operator==` only — no `operator<`
- `EnumType::backing_type_` (`TypeKind`) — defaults to `I32_t`, configurable via `: u32`/`: i64` etc. syntax
- `EnumType::get_backing_type()` — used by codegen to select correct integer width
- Integer-backed enums flow into their backing type via `compatible_to_from()`
- Negative discriminant values are rejected at parse time (TokSUB check in ParseEnumDef)
- `variant_constructors` map: `variant_name → (enum_type, index)` — used for generic enum fallback and `VariableExprAST` unit variant lookup
- Variant resolution in `synthesize(CallExprAST*)`: look up enum via `get_typename_type(module)`, fall back to `variant_constructors` for generics, then find variant by name
- See `type_checking.md` for full enum type checker details (EnumType, variant constructors, case expressions, exhaustiveness)

## Scope Generator

- `preorder_walk(ProgramAST*)` → registers variant names in scope, populates `variant_to_enum`
- `postorder_walk(CallExprAST*)` → rewrites unqualified variant calls to qualified form, then returns early
- For already-qualified names → checks if prefix is an enum name in scope, defers to type checker
- `variant_to_enum`: `std::map<std::string, std::string>` in `ScopeGeneratorVisitor.h`
- `visit(CallExprAST*)` has early return for `is_enum_constructor`

## Codegen

See `codegen.md` for full details. Summary:

| Backend | Key details |
|---|---|
| LLVM | `emitEnumConstructor` in `FunctionCodegen.cpp`; `TypeConverter::register_enum_type`/`get_enum_type`; forward declaration pass registers LLVM struct types |
| MLIR | `enumTypes` map in `MLIRGenImpl`; `emitEnumConstructor` in `MLIRGenExpr.cpp` (LLVM dialect); `getTypeSize` → `4 + max_payload_size`; `convertType` looks up `enumTypes` |
| Case (LLVM) | `switch i32 %tag` + basic blocks per arm + PHI merge; payload via byte-offset GEP |
| Case (MLIR) | Cascading `cmpi + cf::CondBranchOp`; merge blocks with block arguments |

## Generic Enums

- `enum Option<T> = Some(T) \| None;` — type params parsed same as generic functions
- Generic enums stored in `generic_enum_defs`; `GenericTypeExprAST` triggers instantiation via `Monomorphizer::instantiate_enum()`
- Monomorphized enum defs inserted at front of `DefinitionVec`
- See `type_checking.md` for generic enum instantiation details

## Exhaustiveness Checking

- After processing all case arms, collects `covered_indices` set
- If no wildcard `_` present and `covered_indices.size() < variant_count()` → error listing missing variants
- Duplicate variant patterns are warned

## Parsing

See `lex_and_parse.md` for full details. Tokens: `TokEnum`, `TokCase`, `TokFatArrow` (`=>`). Variants pipe-separated via `TokORLogical`. Patterns support qualified, unqualified (rewritten), wildcard `_`, and payload bindings.
