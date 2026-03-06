# sammine-lang Language Reference

sammine-lang is a compiled programming language built with C++23 and LLVM/MLIR. It features type inference, generics via monomorphization, partial application, pipe operators, pointers, arrays, structs, enums, typeclasses, linear types, and an import system.

**Compiler pipeline:** Lexer -> Parser -> AST -> ScopeGenerator -> BiTypeChecker -> Monomorphizer -> GeneralSemantics -> CodegenVisitor (MLIR)

Comments begin with `#` and extend to the end of the line.

---

## 1. Types

### Primitive Types

| Type   | Description                  | Size    |
|--------|------------------------------|---------|
| `i32`  | 32-bit signed integer        | 4 bytes |
| `i64`  | 64-bit signed integer        | 8 bytes |
| `u32`  | 32-bit unsigned integer      | 4 bytes |
| `u64`  | 64-bit unsigned integer      | 8 bytes |
| `f32`  | 32-bit floating point        | 4 bytes |
| `f64`  | 64-bit floating point        | 8 bytes |
| `bool` | Boolean (`true` or `false`)  | 1 byte  |
| `char` | Single character             | 1 byte  |
| `unit` | Unit type (no meaningful value) | 0 bytes |

Strings are C-compatible `ptr<char>` and written as double-quoted literals: `"hello"`.

Char literals use single quotes: `'A'`, `'0'`, `'\n'`.

### Literal Defaults and Suffixes

Unsuffixed integer literals default to `i32`. Unsuffixed float literals default to `f64`.

Suffixes override the default: `42i64`, `42u32`, `100u64`, `3.14f32`.

Polymorphic literals flow into the type expected by an annotation or function parameter:

```sammine
let x: i64 = 7;          # literal 7 becomes i64
let y: f32 = 3.14;       # literal 3.14 becomes f32
let z: i64 = 1 + 2;      # binary expression of literals resolves to i64
```

### Type Aliases

The `type` keyword can define a transparent alias for any type:

```sammine
import std;
type IntAlias = i32;

let main() -> i32 {
  let x: IntAlias = 42;
  std::printf("%d\n", x);
  0
}
```

---

## 2. Variables

### Immutable Bindings

Variables are immutable by default. Declare with `let`:

```sammine
let x: i32 = 5;       # explicit type annotation
let y = 10;            # type inferred as i32
let name = "hello";    # type inferred as ptr<char>
```

Reassigning an immutable variable is a compile-time error.

Variable shadowing is not allowed -- re-declaring a variable name that already exists in an enclosing scope is a compile-time error.

### Mutable Bindings

Use `let mut` for mutable variables:

```sammine
let mut counter: i32 = 0;
counter = counter + 1;         # OK: counter is mutable
```

### Type Inference

The compiler infers types from initializer expressions:

```sammine
let x = 42;           # i32
let y = add(1, 2);    # inferred from return type of add
let z = 3.14;         # f64
```

---

## 3. Functions

### Definition

Functions are declared with `let name(params) -> ReturnType { body }`:

```sammine
let add(a: i32, b: i32) -> i32 {
  a + b                          # implicit return (last expression)
}

let greet() {                    # return type omitted means unit
  std::printf("hello\n");
}
```

### Mutable Parameters

Function parameters can be declared `mut` to allow reassignment within the function body:

```sammine
let countdown(mut n: i32) -> i32 {
  while n > 0 {
    std::printf("%d\n", n);
    n = n - 1;
  };
  0
}
```

### `main` Parameters

The `main` function can optionally accept command-line arguments:

```sammine
let main(argc: i32, argv: ptr<ptr<char>>) -> i32 {
  std::printf("argc = %d\n", argc);
  0
}
```

### Implicit Return

The last expression in a function body is the return value. An explicit `return` statement is also supported:

```sammine
let fib(x: i32) -> i32 {
  if x == 0 || x == 1 {
    x                            # implicit return from if-branch
  } else {
    fib(x - 1) + fib(x - 2)     # implicit return from else-branch
  }
}
```

### Recursion and Mutual Recursion

Functions can call themselves or each other. Forward declarations are handled automatically:

```sammine
let is_even(n: i32) -> i32 {
  if n == 0 { 1 }
  else { is_odd(n - 1) }
}

let is_odd(n: i32) -> i32 {
  if n == 0 { 0 }
  else { is_even(n - 1) }
}
```

### C FFI with `reuse`

The `reuse` keyword declares a C function binding:

```sammine
reuse sqrt(x: f64) -> f64;                     # bind to C sqrt()
reuse printf(fmt: ptr<char>, ...) -> i32;       # varargs supported
```

Imported modules can re-export C bindings with `export reuse`:

```sammine
# In stdlib/std.mn:
export reuse printf(fmt: ptr<char>, ...) -> i32;
```

---

## 4. Control Flow

### If/Else

`if/else` is an expression that returns a value. Both branches must end with a semicolon when used as a statement:

```sammine
# As a statement
if x > 3 {
  std::printf("big\n");
} else {
  std::printf("small\n");
};

# As an expression (returns a value)
let sign = if x > 0 { 1 }
           else if x == 0 { 0 }
           else { -1 };
```

`if` without `else` is allowed for side-effectful code:

```sammine
if x > 3 {
  std::printf("yes\n");
};
```

### While Loops

```sammine
let mut i: i32 = 5;
while i > 0 {
  std::printf("%d\n", i);
  i = i - 1;
};
```

Loops can be nested:

```sammine
let mut i: i32 = 1;
while i <= 2 {
  let mut j: i32 = 1;
  while j <= 2 {
    std::printf("%d %d\n", i, j);
    j = j + 1;
  };
  i = i + 1;
};
```

There are no `for` loops yet.

---

## 5. Operators

### Arithmetic

`+`, `-`, `*`, `/`, `%` (modulo). Work on integer and float types (except `%` which is integer-only):

```sammine
let a: i32 = 10;
let b: i32 = 3;
std::printf("%d\n", a + b);    # 13
std::printf("%d\n", a % b);    # 1
```

### Comparison

`==`, `!=`, `<`, `>`, `<=`, `>=`. Return `bool`:

```sammine
if a < b { std::printf("less\n"); }
else { std::printf("not less\n"); };
```

### Logical

`&&` (and), `||` (or). Short-circuit evaluation:

```sammine
if x > 0 && x < 100 {
  std::printf("in range\n");
};
```

### Bitwise

`&` (and), `|` (or), `^` (xor), `<<` (left shift), `>>` (right shift). Work on integer types:

```sammine
let a: i32 = 10;
let b: i32 = 12;
std::printf("%d\n", a & b);     # 8
std::printf("%d\n", a | b);     # 14
std::printf("%d\n", a ^ b);     # 6
std::printf("%d\n", a << 2);    # 40
std::printf("%d\n", a >> 2);    # 2
```

### Unary

`-x` negates a numeric value. Unsigned types cannot be negated (compile-time error):

```sammine
let x: i32 = 5;
std::printf("%d\n", -x);        # -5
std::printf("%d\n", -(-x));     # 5
```

### Operator Precedence

From lowest to highest precedence:

| Prec | Operators | Description |
|------|-----------|-------------|
| 1    | `\|>`     | pipe |
| 2    | `=`       | assignment |
| 3    | `\|`      | bitwise OR |
| 4    | `\|\|`    | logical OR |
| 5    | `&`       | bitwise AND |
| 6    | `^`       | bitwise XOR |
| 7    | `&&`      | logical AND |
| 10   | `==` `!=` `<` `>` `<=` `>=` | comparison |
| 15   | `<<` `>>` | shift |
| 20   | `+` `-`   | additive |
| 40   | `*` `/` `%` | multiplicative |

---

## 6. Structs

### Definition and Construction

```sammine
struct Point { x: i32, y: f64, };

let main() -> i32 {
  let p: Point = Point { x: 42, y: 3.14 };
  std::printf("x = %d\n", p.x);
  std::printf("y = %f\n", p.y);
  return 0;
}
```

### Nested Structs

```sammine
struct Inner { value: i32, };
struct Outer { inner: Inner, x: i32, };

let main() -> i32 {
  let i: Inner = Inner { value: 42 };
  let o: Outer = Outer { inner: i, x: 10 };
  std::printf("%d\n", o.inner.value);    # 42
  return 0;
}
```

### Structs as Parameters and Return Values

```sammine
struct Pair { a: i32, b: i32, };

let make_pair(x: i32, y: i32) -> Pair {
  return Pair { a: x, b: y };
}
```

### Generic Structs

```sammine
struct Box<T> {
  value: T,
};

let main() -> i32 {
  let b = Box<i32>{ value: 42 };
  std::printf("%d", b.value);       # 42
  return 0;
}
```

Multi-parameter generic structs:

```sammine
struct Pair<T, U> {
  first: T,
  second: U,
};

let p = Pair<i32, i32>{ first: 10, second: 20 };
```

---

## 7. Enums

### Unit Variants

```sammine
type Color = Red | Green | Blue;

let c: Color = Color::Green;
```

### Payload Variants

Variants can carry data:

```sammine
type Shape = Circle(i32) | Rect(i32, i32) | Point;

let s: Shape = Shape::Circle(3);
let r: Shape = Shape::Rect(2, 4);
let p: Shape = Shape::Point;
```

### Integer-Backed Enums

Variants with explicit integer discriminants:

```sammine
type OpenFlags = RDONLY(0) | WRONLY(1) | CREAT(64);
```

With a backing type annotation:

```sammine
type Flags: u32 = Read(1) | Write(2) | Exec(4);

let r: Flags = Read;
let w: Flags = Write;
```

Integer-backed enums support bitwise operations:

```sammine
type Flags = Read(1) | Write(2) | Exec(4);

let rw: Flags = Read | Write;        # bitwise OR: value 3
let all: Flags = Read | Write | Exec; # value 7
```

### Case/Match Expressions

Pattern match on enums using `case`:

```sammine
type Color = Red | Green | Blue;

let color_name(c: Color) -> i32 {
  case c {
    Color::Red => std::printf("red\n"),
    Color::Green => std::printf("green\n"),
    Color::Blue => std::printf("blue\n"),
  };
  return 0;
}
```

Destructure payload variants:

```sammine
type Shape = Circle(i32) | Rect(i32, i32) | Point;

let describe(s: Shape) -> i32 {
  case s {
    Shape::Circle(r) => std::printf("circle r: %d\n", r),
    Shape::Rect(w, h) => std::printf("rect w: %d, h: %d\n", w, h),
    Shape::Point => std::printf("point\n"),
  };
  return 0;
}
```

Unqualified variant names work when unambiguous, both in patterns and constructors:

```sammine
let c: Color = Red;              # unqualified constructor (no Color:: needed)

case s {
  Circle(r) => std::printf("circle r: %d\n", r),
  Rect(w, h) => std::printf("rect w: %d, h: %d\n", w, h),
  Point => std::printf("point\n"),
};
```

Wildcard `_` catches all remaining patterns:

```sammine
case c {
  Color::Red => { std::printf("is red\n"); return 1; },
  _ => { std::printf("not red\n"); return 0; },
}
```

### Generic Enums

Enums can be parameterized with type variables, just like structs:

```sammine
type Option<T> = Some(T) | None;

let main() -> i32 {
  let x: Option<i32> = Option<i32>::Some(42);
  case x {
    Some(v) => std::printf("%d\n", v),
    None => std::printf("none\n"),
  };
  return 0;
}
```

`Option<T>` is provided by the standard library and available in all programs without an import (see [Imports](#16-imports)).

---

## 8. Generics

Generic functions are monomorphized at compile time -- a separate copy is generated for each concrete type used.

### Generic Functions

```sammine
let identity<T>(x: T) -> T { x }

let main() -> i32 {
  let a = identity(42);           # inferred: identity<i32>
  let b = identity(3.14);         # inferred: identity<f64>
  std::printf("%d %.2f", a, b);   # 42 3.14
  return 0;
}
```

### Explicit Type Arguments

```sammine
let a = identity<i32>(42);
let b = identity<f64>(3.14);
```

### Generic Functions Calling Generic Functions

```sammine
let identity<T>(x: T) -> T { x }

let apply_identity<T>(x: T) -> T {
  identity<T>(x)
}

let main() -> i32 {
  std::printf("%d\n", apply_identity<i32>(42));
  return 0;
}
```

### Generic Structs

See the [Structs](#6-structs) section for `struct Box<T>` and `struct Pair<T, U>` examples.

---

## 9. Typeclasses

Typeclasses define interfaces that can be implemented for different types.

### Definition and Instances

```sammine
typeclass MySize<T> {
  mysize() -> i64;
}

instance MySize<i32> {
  let mysize() -> i64 { 4 }
}

instance MySize<i64> {
  let mysize() -> i64 { 8 }
}

let main() -> i32 {
  std::printf("%ld\n", mysize<i32>());    # 4
  std::printf("%ld\n", mysize<i64>());    # 8
  return 0;
}
```

### Qualified Call Syntax

Typeclass methods can be called with either syntax:

```sammine
mysize<i32>()                 # unqualified with type arg
MySize<i32>::mysize()         # qualified: ClassName<Type>::method()
```

### Multi-Parameter Typeclasses

```sammine
typeclass Converter<From, To> {
  Converter(x: From) -> To;
}

instance Converter<Celsius, Fahrenheit> {
  let Converter(x: Celsius) -> Fahrenheit {
    Fahrenheit { val: x.val * 9 / 5 + 32 }
  }
}
```

### Builtin Typeclasses

**`Sized<T>`** -- returns the byte size of a type at compile time:

```sammine
std::printf("%ld\n", Sized<i32>());     # 4
std::printf("%ld\n", Sized<i64>());     # 8
std::printf("%ld\n", Sized<f64>());     # 8
std::printf("%ld\n", Sized<bool>());    # 1
std::printf("%ld\n", Sized<char>());    # 1
```

**`Hash<T>`** -- user-defined hash function:

```sammine
struct Point { x: i32, y: i32, };

instance Hash<Point> {
  let Hash(val: Point) -> u64 { 42u64 }
}

let h = Hash<Point>(Point { x: 1, y: 2 });
```

**`Add<T>`**, **`Sub<T>`**, **`Mul<T>`**, **`Div<T>`**, **`Mod<T>`** -- operator overloading:

```sammine
struct Vec2 { x: i32, y: i32, };

instance Add<Vec2> {
  let Add(a: Vec2, b: Vec2) -> Vec2 {
    Vec2 { x: a.x + b.x, y: a.y + b.y }
  }
}

let v1: Vec2 = Vec2 { x: 10, y: 15 };
let v2: Vec2 = Vec2 { x: 20, y: 25 };
let v3: Vec2 = v1 + v2;                 # uses Add<Vec2>
std::printf("%d %d\n", v3.x, v3.y);     # 30 40
```

**`Indexer<Collection, Element>`** -- custom indexing:

```sammine
struct IntList { a: i32, b: i32, c: i32, };

instance Indexer<IntList, i32> {
  let Indexer(coll: IntList, idx: i64) -> i32 {
    if idx == 0i64 { coll.a }
    else if idx == 1i64 { coll.b }
    else { coll.c }
  }
}
```

---

## 10. Tuples

Tuple literals group multiple values. Destructure with pattern matching on `let`:

```sammine
let t = (42, true);          # type: (i32, bool)
let (a, b) = t;              # destructuring
std::printf("%d\n", a);      # 42
std::printf("%d\n", b);      # 1 (true printed as integer)
```

---

## 11. Arrays

Fixed-size, stack-allocated arrays with compile-time known length.

### Declaration and Indexing

```sammine
let a: [i32; 3] = [10, 20, 30];
std::printf("%d\n", a[0]);           # 10
std::printf("%d\n", a[1]);           # 20
std::printf("%d\n", a[2]);           # 30
```

### Array Length

```sammine
let a: [i32; 5] = [1, 2, 3, 4, 5];
std::printf("%d\n", len(a));         # 5
```

### Mutable Array Elements

```sammine
let mut a: [i32; 3] = [10, 20, 30];
a[1] = 99;
std::printf("%d\n", a[1]);          # 99
```

### Bounds Checking

Out-of-bounds access with a constant index is caught at compile time:

```sammine
let a: [i32; 3] = [10, 20, 30];
std::printf("%d\n", a[5]);
# Error: Array index out of bounds: index 5 on array of size 3
```

---

## 12. Pointers and Memory

sammine-lang has two pointer kinds: **linear** (`'ptr<T>`) for heap-allocated memory that must be freed, and **non-linear** (`ptr<T>`) for stack references.

### Heap Allocation (Linear Pointers)

`alloc<T>(count)` allocates memory on the heap and returns a linear `'ptr<T>`. Linear pointers must be consumed exactly once -- via `free()`, a move, or a return:

```sammine
let p: 'ptr<i32> = alloc<i32>(1);    # allocate 1 element
*p = 42;                              # write through pointer
let y: i32 = *p;                      # dereference (read)
free(p);                              # deallocate (consumes p)
std::printf("%d\n", y);               # 42
```

Failing to free a linear pointer is a compile-time error:

```sammine
let p = alloc<i32>(1);
*p = 42;
return 0;
# Error: Linear variable 'p' must be consumed before scope exit (use free() to deallocate)
```

### Address-Of (Non-Linear Pointers)

`&x` takes the address of a stack variable and returns a non-linear `ptr<T>`:

```sammine
let x: i32 = 42;
let p: ptr<i32> = &x;
let y: i32 = *p;                     # dereference
std::printf("%d\n", y);              # 42
```

### Pointer Indexing

Heap-allocated buffers can be indexed like arrays:

```sammine
let mut p: 'ptr<i32> = alloc<i32>(3);
p[0] = 10;
p[1] = 20;
p[2] = 42;
std::printf("%d\n", p[0]);           # 10
std::printf("%d\n", p[2]);           # 42
free(p);
```

### Ownership Transfer (Move)

Assigning a linear pointer to a new variable moves ownership:

```sammine
let p = alloc<i32>(1);
*p = 55;
let q = p;            # p is consumed, q now owns the memory
let v: i32 = *q;
free(q);              # must free q, not p
```

### Returning Linear Pointers

Functions can transfer ownership by returning a linear pointer:

```sammine
let make_ptr() -> 'ptr<i32> {
  let p = alloc<i32>(1);
  *p = 123;
  return p;           # ownership transferred to caller
}

let main() -> i32 {
  let p = make_ptr();
  let v: i32 = *p;
  free(p);            # caller must free
  std::printf("%d\n", v);
  return 0;
}
```

---

## 13. Pipe Operator

The pipe operator `|>` passes the left-hand value as the first argument to the right-hand function:

```sammine
let square(x: i32) -> i32 { x * x }

std::printf("%d\n", 5 |> square);     # square(5) = 25
```

With additional arguments:

```sammine
let add(a: i32, b: i32) -> i32 { a + b }

std::printf("%d\n", 5 |> add(3));     # add(5, 3) = 8
```

Pipes chain left-to-right:

```sammine
let square(x: i32) -> i32 { x * x }
let double(x: i32) -> i32 { x + x }
let add(a: i32, b: i32) -> i32 { a + b }

let result = 5 |> square |> double |> add(1);
std::printf("%d\n", result);           # ((5*5)*2)+1 = 51
```

---

## 14. Partial Application

Calling a function with fewer arguments than it expects creates a closure:

```sammine
let add(a: i32, b: i32) -> i32 { a + b }

let add5: (i32) -> i32 = add(5);     # partially apply first arg
std::printf("%d\n", add5(3));         # 8
std::printf("%d\n", add5(7));         # 12
```

Multi-argument partial application:

```sammine
let add3(a: i32, b: i32, c: i32) -> i32 { a + b + c }

let add1_2: (i32) -> i32 = add3(1, 2);
std::printf("%d\n", add1_2(7));       # 10
```

Partial application works with higher-order functions:

```sammine
let apply(f: (i32) -> i32, x: i32) -> i32 { f(x) }

std::printf("%d\n", apply(add(10), 32));    # 42
```

Note: Closures created by partial application are stack-allocated and cannot escape their defining scope.

---

## 15. Higher-Order Functions

Functions are first-class values. Function types are written as `(ParamTypes) -> ReturnType`:

### Functions as Values

```sammine
let inc(x: i32) -> i32 { x + 1 }

let f: (i32) -> i32 = inc;
std::printf("%d\n", f(41));           # 42
```

### Functions as Parameters

```sammine
let square(x: i32) -> i32 { x * x }

let apply(f: (i32) -> i32, x: i32) -> i32 { f(x) }

std::printf("%d\n", apply(square, 5));    # 25
```

### Generic Higher-Order Functions

```sammine
let apply<T, U>(f: (T) -> U, x: T) -> U {
  f(x)
}
```

---

## 16. Imports

### Basic Imports

```sammine
import std;                            # import the std module
std::printf("hello %d\n", 42);        # qualified access
```

### Aliased Imports

```sammine
import math as m;
m::add(3, 4);
```

### Without Alias

```sammine
import math;
math::multiply(3, 4);
```

### Exporting from Modules

Use `export` to make definitions visible to importers. `export` works with functions, structs, enums, and C bindings:

```sammine
# In math.mn:
export let add(a: i32, b: i32) -> i32 { a + b }
export struct Point { x: i32, y: i32, };
export type Color = Red | Green | Blue;
export reuse printf(fmt: ptr<char>, ...) -> i32;
```

### Standard Library Modules

- **`definitions`** (auto-loaded) -- automatically included in all executables. Provides builtin typeclasses (`Sized`, `Add`, `Sub`, `Mul`, `Div`, `Mod`, `Hash`, `Indexer`) with instances for primitive types, and `Option<T>`.
- **`std`** -- provides `printf`
- **`io`** -- provides `printf`, `print`, `println`, `eprint`, `eprintln`, `print_i32`, `print_i64`, `print_f64`, `print_char`, `print_bool`, `read_line`, `read_i32`, `read_i64`, `read_f64`, file I/O (`open`, `close`, `file_read_line`, `file_write`, `read_file`, `write_file`, `append_file`)
- **`vec`** -- provides `Vec<T>` dynamic array (see below)
- **`str`** -- provides `String` type with `new` and `delete`

---

## 17. Vec\<T\>

`Vec<T>` is a generic, auto-growing dynamic array from the standard library.

```sammine
import std;
import vec;

let main() -> i32 {
  let v0 = vec::new<i32>(4);             # create with initial capacity 4
  let v1 = vec::push<i32>(v0, 10);       # push returns new Vec
  let v2 = vec::push<i32>(v1, 20);
  let v3 = vec::push<i32>(v2, 30);
  std::printf("%d %d %d", v3.data[0], v3.data[1], v3.data[2]);  # 10 20 30
  vec::delete<i32>(v3);                   # must free (linear data pointer)
  return 0;
}
```

### API

| Function | Signature | Description |
|----------|-----------|-------------|
| `vec::new<T>(cap)` | `(i64) -> Vec<T>` | Create a new Vec with given capacity |
| `vec::push<T>(v, val)` | `(Vec<T>, T) -> Vec<T>` | Append an element, auto-grows if full |
| `vec::delete<T>(v)` | `(Vec<T>) -> unit` | Free the underlying memory |

### Vec\<T\> Structure

```sammine
struct Vec<T> {
  data: 'ptr<T>,     # linear pointer to heap buffer
  length: i64,       # current number of elements
  cap: i64,          # allocated capacity
};
```

`push` is a functional-style operation: it returns a new `Vec<T>` value. When capacity is exceeded, the buffer is doubled automatically. Each intermediate `Vec` value is consumed (moved) by the next `push` call, so only the final `Vec` needs an explicit `delete`.

### Auto-Growth Example

```sammine
import vec;

let v0 = vec::new<i32>(2);     # capacity 2
let v1 = vec::push<i32>(v0, 0);
let v2 = vec::push<i32>(v1, 1);
let v3 = vec::push<i32>(v2, 2);  # triggers growth to capacity 4
# ... keep pushing, grows as needed
vec::delete<i32>(v3);
```
