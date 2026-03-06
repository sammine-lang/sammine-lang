# sammine-lang

> A compiled programming language built with C++23 and LLVM/MLIR. Features: type inference, generics, partial application, pipe operators, linear types, structs, enums, typeclasses, and more. Follow along via Jasmine's [blog](https://badumbatish.github.io/blog).

![](https://github.com/badumbatish/sammine-lang/blob/main/img.png)
How I pictured sammine-lang in my head

## Language Features

```sammine
# Line comments start with #
# Types: i32, i64, u32, u64, f32, f64, bool, char, unit

import std;                                   # import standard library (provides printf)
import vec;                                   # dynamic arrays

reuse sqrt(x: f64) -> f64;                   # C FFI binding

# --- Functions ---
let add(a: i32, b: i32) -> i32 {             # function definition
  a + b                                       # implicit return (last expression)
}

let fib(x: i32) -> i32 {                     # recursion, if/else as expression
  if x == 0 || x == 1 {                      # logical operators: && ||
    x
  } else {
    fib(x - 1) + fib(x - 2)                  # arithmetic: + - * / %
  }
}

# --- Generics (monomorphized) ---
let identity<T>(x: T) -> T { x }             # explicit type parameters

let apply<T, U>(f: (T) -> U, x: T) -> U {    # higher-order + generic
  f(x)                                        # (T) -> U is a function type
}

# --- Structs ---
struct Point { x: i32, y: i32, };

# --- Enums ---
type Shape = Circle(i32) | Rect(i32, i32) | Dot;

# --- Typeclasses ---
typeclass MySize<T> {
  mysize() -> i64
}

instance MySize<Point> {
  let mysize() -> i64 { 8 }
}

let main() -> i32 {
  # --- Variables ---
  let x: i32 = 5;                            # immutable, type-annotated
  let y = 10;                                # type inference
  let mut counter: i32 = 0;                  # mutable binding
  counter = counter + 1;                     # assignment (mut only)

  # --- Control flow ---
  let sign = if x > 0 { 1 }                 # if/else is an expression
             else if x == 0 { 0 }           # comparison: < <= > >= == !=
             else { -1 };                    # unary negation

  let mut i: i32 = 3;                        # while loops
  while i > 0 {
    std::printf("%d\n", i);
    i = i - 1;
  };

  # --- Arrays ---
  let a: [i32; 3] = [10, 20, 30];           # fixed-size array literal
  let first = a[0];                          # indexing
  let size = len(a);                         # array length

  # --- Linear pointers & memory ---
  let p = alloc<i32>(1);                     # heap alloc -> 'ptr<i32> (linear)
  *p = 100;                                  # write through pointer
  let val = *p;                              # dereference
  free(p);                                   # must free linear pointers

  let q: ptr<i32> = &x;                     # address-of (non-linear stack ptr)

  # --- Vec<T> (dynamic array) ---
  let v0 = vec::new<i32>(4);                 # create with capacity 4
  let v1 = vec::push<i32>(v0, 10);           # push consumes v0, returns new vec
  let v2 = vec::push<i32>(v1, 20);
  let v3 = vec::push<i32>(v2, 30);
  std::printf("%d %d\n", v3.data[0], v3.data[1]);
  vec::delete<i32>(v3);                      # free the underlying buffer

  # --- Structs ---
  let pt = Point { x: 10, y: 20 };
  std::printf("x=%d y=%d\n", pt.x, pt.y);

  # --- Enums + pattern matching ---
  let s: Shape = Shape::Circle(5);
  case s {
    Circle(r) => std::printf("circle r=%d\n", r),
    Rect(w, h) => std::printf("rect %dx%d\n", w, h),
    Dot => std::printf("dot\n"),
  };

  # --- Tuples ---
  let t = (42, true);                        # tuple literal
  let (a2, b2) = t;                          # destructuring

  # --- Pipe operator ---
  let r1 = 5 |> add(3);                     # x |> f(y) becomes f(x, y)
  let r2 = 5 |> identity<i32> |> add(3);    # chained pipes

  # --- Partial application ---
  let add5: (i32) -> i32 = add(5);          # fewer args -> closure
  let eight = add5(3);                       # call the closure

  # --- Higher-order functions ---
  let f: (i32) -> i32 = add5;              # function as value
  let res = apply<i32, i32>(f, 42);         # pass function as argument

  std::printf("%d\n", fib(9));
  return 0;                                  # explicit return
}
```

## Documentation

- [Language Reference](docs/language-reference.md) — Full guide to every language feature with examples
- [Architecture Guide](docs/architecture.md) — Compiler pipeline, source layout, and developer guide
- [Formal Grammar](docs/grammar.md) — BNF-style grammar specification

## Dev

### MacOS Configurations

Install llvm (includes MLIR)

```bash
brew install llvm
```
Run
```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug [-DSAMMINE_TEST=ON/OFF] [-DCMAKE_LINKER_TYPE=MOLD]
```

for configuration. We need [LLVM & MLIR](https://llvm.org/) and [lit](https://pypi.org/project/lit/).

Run

```bash
cmake --build build -j
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
./build/bin/sammine --help

Usage: sammine [--help] [--version] [[--file VAR]|[--str VAR]] [--check] [--llvm-ir] [--ast-ir] [--diagnostics VAR]

Optional arguments:
  -h, --help      shows help message and exits
  -v, --version   prints version information and exits
  -f, --file      An input file for compiler to scan over.
  -s, --str       An input string for compiler to scan over.
  --check         Performs compiler check only, no codegen

Diagnostics related options (detailed usage):
   --llvm-ir      sammine compiler spits out LLVM-IR to stdout
   --ast-ir       sammine compiler spits out the internal AST to stdout
   --diagnostics  sammine compiler spits out diagnostics for sammine-lang developers.
                  Use with value for logging: --diagnostics=stages;lexer;parser. Default value is none
   --time         Print compilation timing. Also accepts: sparse (per-phase table), coarse (per-phase + all LLVM passes)
   --dev          Show compiler source locations in error messages (for developers)
```
