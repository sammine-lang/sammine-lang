# sammine-lang

> A compiled programming language built with C++23 and LLVM/MLIR. Features: type inference, generics, partial application, pipe operators, linear types, structs, enums, typeclasses, and more. Follow along via Jasmine's [blog](https://badumbatish.github.io/blog).

![](https://github.com/badumbatish/sammine-lang/blob/main/img.png)
How I pictured sammine-lang in my head

## Language Features

```sammine
// RUN: %sammine_jit --file %full | %check %full
// CHECK:      34
// CHECK-NEXT: (30, 40)
// CHECK-NEXT: sizeof i32 = 4
// CHECK-NEXT: sizeof i32 = 4
// CHECK-NEXT: A

// Line comments start with //
// Types: i32, i64, f64, bool, char, unit, etc

import std;                                   // import standard library (provides printf)

reuse sqrt(x: f64) -> f64;                   // C function binding

let add(a: i32, b: i32) -> i32 {             // function definition
  a + b                                       // implicit return (last expression)
}

let fib(x: i32) -> i32 {                     // recursion, if/else as expression
  if x == 0 || x == 1 {                      // logical operators: && ||
    x
  } else {
    fib(x - 1) + fib(x - 2)                  // arithmetic: + - * / %
  }
}

let identity<T>(x: T) -> T { x }             // generics (monomorphized)

let apply<T, U>(f: (T) -> U, x: T) -> U {   // higher-order + generic
  f(x)                                       // (T) -> U is a function type
}

// --- Type aliases ---
type Int = i32;                                   // transparent alias for i32

// --- Structs ---
struct Vec2 { x: i32, y: i32, };

// --- Operator overloading via typeclasses ---
instance Add<Vec2> {
  let Add(a: Vec2, b: Vec2) -> Vec2 {
    Vec2 { x: a.x + b.x, y: a.y + b.y }
  }
}

let main() -> i32 {
  // --- Variables ---
  let x: Int = 5;                            // using type alias
  let y = 10;                                // type inference
  // --- Control flow ---
  let sign = if x > 0 { 1 }                 // if/else is an expression
             else if x == 0 { 0 }           // comparison: < <= > >= == !=
             else { -1 };                    // unary negation

  // --- Arrays ---
  let a: [3]i32 = [10, 20, 30];           // fixed-size array literal
  let first = a[0];                          // indexing
  let size = len(a);                         // array length

  // --- Tuples ---
  let t = (100, true);                        // tuple literal (i32, bool)
  let (tx, ty) = t;                           // destructuring let

  // --- Pointers & memory ---
  let p: 'ptr<i32> = alloc<i32>(1);         // heap allocation (count, linear)
  *p = 42;                                   // write through pointer
  let val = *p;                              // dereference
  free(p);                                   // manual deallocation

  let q: ptr<i32> = &x;                     // address-of (stack pointer)
  let pp: ptr<ptr<i32>> = &q;               // nested pointers

  // --- Pipe operator ---
  let r1 = 5 |> add(3);                     // x |> f(y) becomes f(x, y)
  let r2 = 5 |> identity |> add(3);         // chained pipes

  // --- Partial function application ---
  let add5 = add(5);                         // fewer args -> closure
  let eight = add5(3);                       // call the partially applied fn

  // --- Higher-order functions ---
  let f: (i32) -> i32 = add5;               // function as value
  let res = apply(f, 42);                   // pass function as argument

  // --- Generics (monomorphized) ---
  let a_i32 = identity(42);                 // inferred: identity.i32
  let a_f64 = identity<f64>(3.14);          // explicit type arg: identity.f64

  // --- Structs + operator overloading ---
  let v1: Vec2 = Vec2 { x: 10, y: 15 };    // struct literal
  let v2: Vec2 = Vec2 { x: 20, y: 25 };
  let v3: Vec2 = v1 + v2;                   // uses Add<Vec2> instance

  // --- Sizeof (built-in typeclass) ---
  let sz = Sized<i32>();                     // compile-time size info
  let sz2 = Sized<i32>::Sized();             // qualified typeclass call syntax

  // --- Chars ---
  let ch: char = 'A';

  std::printf("%d\n", fib(9));              // 34
  std::printf("(%d, %d)\n", v3.x, v3.y);   // (30, 40)
  std::printf("sizeof i32 = %ld\n", sz);   // sizeof i32 = 4
  std::printf("sizeof i32 = %ld\n", sz2);  // sizeof i32 = 4 (qualified syntax)
  std::printf("%c\n", ch);                  // A
  return 0;                                  // explicit return
}
```

## Documentation

- [Language Reference](docs/language-reference.md) — Full guide to every language feature with examples
- [Architecture Guide](docs/architecture.md) — Compiler pipeline, source layout, and developer guide
- [Formal Grammar](docs/grammar.md) — BNF-style grammar specification

## Dev

### Prerequisites: Build LLVM from submodule

LLVM is included as a git submodule at `externals/llvm-project`. After cloning, initialize it:

```bash
git submodule update --init --recursive
```

Then configure LLVM with roughly these arguments

```bash
cmake -S externals/llvm-project/llvm -B externals/llvm-project/build -G Ninja \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DLLVM_CCACHE_BUILD=true \
  -DLLVM_USE_LINKER=lld \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLLVM_ENABLE_PROJECTS="mlir;llvm" \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ 
```

### Configurations

We need [lit](https://pypi.org/project/lit/).

Point `LLVM_DIR` and `MLIR_DIR` to your local LLVM build:

```bash
cmake -S . -B build \
  -DLLVM_DIR=externals/llvm-project/build/lib/cmake/llvm \
  -DMLIR_DIR=externals/llvm-project/build/lib/cmake/mlir \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSAMMINE_TEST=ON
```

Optional flags:
- `-DSAMMINE_SANITIZE=ON` — Enable ASan + UBSan
- `-DSAMMINE_TRACY=ON` — Enable [Tracy](https://github.com/wolfpld/tracy) profiler instrumentation (uses `TRACY_ON_DEMAND`, no overhead until the Tracy GUI connects)

Run

```bash
cmake --build externals/llvm-project/build -j && cmake --build build -j
```

and

```bash
cmake --build build -j --target unit-tests e2e-tests
```
in the project root folder to cycle through the development process.

Run
```bash
rm -rf build
```
to erase the build folder (similar to make clean).

## Compiler CLI

```
  -f, --file      An input file for compiler to scan over.
  -s, --str       An input string for compiler to scan over.
  --check         Performs compiler check only, no codegen
  --jit           JIT execute the program directly (only effective with main function)
  --jit-args      Arguments to pass to the JIT-executed program (repeatable)
  -O              Output directory for build artifacts (.so, .a, .exe)
  -I              Add directory to import search path (repeatable)
  --lib           Emit library output. Values: static (.a) or shared (.so)
  --llvm-ir       Emit LLVM IR. Values: pre, post, or diff
  --mlir-ir       Dump MLIR before lowering to LLVM IR
  --ast-ir        Dump the internal AST to stdout
  --diagnostics   Developer diagnostics. Values: stages;lexer;parser or 'dev'
  --time          Print compilation timing. Values: simple, sparse, coarse
```
