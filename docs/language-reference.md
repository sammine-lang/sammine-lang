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
| `string` | Text that lives as long as the program | a pointer |
| `Formatter` | Where a `Format` instance writes, and how (see Print) | 16 bytes |
| `unit` | Unit type (no meaningful value) | 0 bytes |

A double-quoted literal, `"hello"`, is a `string`: its text lives as long as
the program and ends where C reads it to, so it is passed to C as it is. A
string is a value like a number: it is bound, passed, returned and kept in a
struct. It is not a pointer to a `char`: nothing says where a `ptr<char>`'s
text ends, so one is no string and doesn't print as one.

```sammine
let name = "ada";                        // string
struct Person { name: string, age: i32, };
let chars = name as ptr<char>;           // a view of its chars, always allowed
count(name);                             // or lent as one, to a ptr<char> parameter
let unsafe text(p: ptr<char>) -> string { p as string }   // unsafe: p must end
```

Strings compare by what they say (`==`, `!=`, `<` and the rest, in C's
`strcmp` order) and hash by it, so a string is a `Map`'s key.
`str::count(s)` is how many chars one has.

`str::String` is text a program builds, owned and growable (see the `str`
module).

Char literals use single quotes: `'A'`, `'0'`, `'\n'`.

### Literal Defaults and Suffixes

Unsuffixed integer literals default to `i32`. Unsuffixed float literals default to `f64`.

Suffixes override the default: `42i64`, `42u32`, `100u64`, `3.14f32`.

Polymorphic literals flow into the type expected by an annotation or function parameter:

```sammine
let x: i64 = 7;          // literal 7 becomes i64
let y: f32 = 3.14;       // literal 3.14 becomes f32
let z: i64 = 1 + 2;      // binary expression of literals resolves to i64
```

### Type Aliases

The `type` keyword can define a transparent alias for any type:

```sammine
import std;
type IntAlias = i32;

let main() -> i32 {
  let x: IntAlias = 42;
  print("{}\n", x);
  0
}
```

---

## 2. Variables

### Immutable Bindings

All variables are immutable. Declare with `let`:

```sammine
let x: i32 = 5;       // explicit type annotation
let y = 10;            // type inferred as i32
let name = "hello";    // type inferred as string
```

### Shadowing

A `let` can reuse a name that is already visible, including the name of a function:

```sammine
let x = 1;
let x = x + 1;         // x is now 2
```

A binding declared in a block ends with that block:

```sammine
let x = 5;
if true {
  let x = 10;
  print("{}\n", x);    // 10
} else {
  print("no\n");
};
print("{}\n", x);      // 5
```

### Type Inference

The compiler infers types from initializer expressions:

```sammine
let x = 42;           // i32
let y = add(1, 2);    // inferred from return type of add
let z = 3.14;         // f64
```

---

## 3. Functions

### Definition

Functions are declared with `let name(params) -> ReturnType { body }`:

```sammine
let add(a: i32, b: i32) -> i32 {
  a + b                          // implicit return (last expression)
}

let greet() {                    // return type omitted means unit
  print("hello\n");
}
```

### `main` Parameters

The `main` function can optionally accept command-line arguments:

```sammine
let main(argc: i32, argv: ptr<ptr<char>>) -> i32 {
  print("argc = {}\n", argc);
  0
}
```

### Implicit Return

The last expression in a function body is the return value. An explicit `return` statement is also supported:

```sammine
let fib(x: i32) -> i32 {
  if x == 0 || x == 1 {
    x                            // implicit return from if-branch
  } else {
    fib(x - 1) + fib(x - 2)     // implicit return from else-branch
  }
}
```

The same holds for any block, a branch's or a case arm's: its value is its
last expression, if no `;` follows it. A block ending in a statement,
`{ f(x); }`, gives `()`, whatever `f` returns, so an arm that only prints
fits beside one that does nothing:

```sammine
case o {
  Some(v) => { print("{}\n", v); },   // ()
  None => {},
};
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
reuse sqrt(x: f64) -> f64;                     // bind to C sqrt()
reuse printf(fmt: string, ...) -> i32;       // varargs supported
```

C never writes into sammine memory, because sammine values are immutable. A C function hands back what it produces as its result, including new pointers, and a `ptr<T>` parameter is one C only reads. The compiler cannot see what C does, so a declaration that says otherwise is a bug in the declaration. Passing an address through `...`, as in `sscanf(s, "%d", &v)`, is an error, since nothing declares how C uses it.

A `reuse` parameter of function type is a function C calls back, which C
takes as a pointer to its code. The function is passed by name, and C calls it
as it calls any C function:

```sammine
reuse pthread_create(t: ptr<u64>, attr: i64, f: (i64) -> i64, arg: i64) -> i32;

let work(arg: i64) -> i64 { arg * 2 }

pthread_create(t, 0i64, work, 42i64);
```

C has nowhere to keep an environment, so only a function named at the call can
be passed: a partial application such as `add(1)`, a binding holding a
function, or a function parameter is an error. A C function that takes one
can't be partially applied or used as a value either, and a function can't go
through `...`. Only numbers, `bool`, `char`, pointers and enums that are
integers go through `...`: no parameter says how a struct, tuple, array or
enum with payloads would be passed. C passes a callback only numbers, `bool`, `char` and pointers,
so its parameters and result are limited to those (or `unit` as the result).

Calling a `reuse` function, or naming one as a value, is allowed only in a
function declared `let unsafe`, like pointer indexing: nothing checks what the
C code does with what it is given. A module wraps its bindings in unsafe
functions, which safe code calls as it calls any function:

```sammine
reuse sqrt(x: f64) -> f64;
export let unsafe root(x: f64) -> f64 { sqrt(x) }
```

Printing needs no binding: `print` is built in (see Print).

Imported modules can re-export C bindings with `export reuse`:

```sammine
// In stdlib/std.mn:
export reuse printf(fmt: string, ...) -> i32;
```

A function from a C library other than the C library itself is declared in a
block that names the library. `"SDL3"` stands for `libSDL3`:

```sammine
export reuse "SDL3" {
  SDL_Init(flags: u32) -> bool;
  SDL_Quit() -> unit;
}
```

A program that uses these, directly or through a module it imports, is
linked with the library. The compiler looks for `libSDL3.so` or `libSDL3.a`
where it looks for modules: the current directory, each `-I` path, the
directory of the module that names it, then the stdlib directory. One it
doesn't find there is left to the system: the linker looks for it as for
`-lSDL3`. Either way, the program names the shared library by its soname,
`libSDL3.so.0`, not where it was found, so at run time the system's loader
finds it where the system keeps libraries (`LD_LIBRARY_PATH` too). The JIT
loads the file found, or `libSDL3.so` by name.

---

## 4. Control Flow

### If/Else

`if/else` is an expression that returns a value. Both branches must end with a semicolon when used as a statement:

```sammine
// As a statement
if x > 3 {
  print("big\n");
} else {
  print("small\n");
};

// As an expression (returns a value)
let sign = if x > 0 { 1 }
           else if x == 0 { 0 }
           else { -1 };
```

`if` without `else` is allowed for side-effectful code:

```sammine
if x > 3 {
  print("yes\n");
};
```

### Loops

sammine has no `while` loop. Bindings are immutable, so a loop condition could only change through memory behind a pointer. Loop over a range with `for`, and write any other loop as recursion, passing the changing state as arguments:

```sammine
let countdown(i: i32) -> unit {
  if i > 0 {
    print("{}\n", i);
    countdown(i - 1)
  }
}
```

### For Loops

A `for` loop runs over an integer range. The end is exclusive:

```sammine
for i in 0..3 {
  print("{}\n", i);    // 0, 1, 2
};
```

### Print

`print("format", args...)` writes the format to stdout, with each `{}` in it
replaced by an argument. It is `()`.

```sammine
print("{} of {} done\n", 3, 10);          // 3 of 10 done
print("{.2} {} {}\n", 0.125, true, 'x');   // 0.13 true x
print("{{}} is a placeholder, 100%\n");    // {} is a placeholder, 100%
```

- **An argument prints through the `Format` instance for its type.** The
  compiler provides the instances for `i32`, `i64`, `u32` and `u64`, for
  `f32` and `f64` (six digits after the point), for `bool` (`true` or
  `false`), `char`, `string`, and an integer-backed enum (its integer). A
  tuple and an array print part by part, `(1, true)` and `[1, 2, 3]`, if
  each part prints. The prelude gives `Option<T>` an instance (`Some(4)`,
  `None`) and `vec` gives `Vec<T>` one.
  Nothing in the format says the type again, so nothing can say it wrong. A
  type with no instance is an error ("print() can't print 'P': no instance of
  Format<P>").
- **`{.N}`** prints a float with `N` digits after the point; an instance of
  your own is handed the digits. **`{{`** and **`}}`** are a brace each.
- **The format is a string literal**, which the compiler reads: a
  placeholder for each argument and an argument for each placeholder, or it
  is an error ("print() has 2 placeholders but 1 argument").
- **It needs no import and no unsafe function.** C's `printf` can still be
  bound with `reuse`, and called from an unsafe function, for what `print`
  doesn't say.

`eprint("format", args...)` is `print` to stderr.

`format("format", args...)` writes nowhere: it gives the text, its chars in
a buffer of their own and how many, `('buf<char>, i64)`, which `str::from`
makes a `String` of:

```sammine
let s = str::from(format("{} of {}", 3, 10));   // the String "3 of 10"
```

A type of your own prints once it has an instance of `Format`. The instance
is given a `Formatter`, which says where to write and how, and writes the
value's parts to it: `write(f, "format", args...)` is `print` to wherever `f`
says.

```sammine
struct Point { x: i32, y: i32, };

instance Format<Point> {
  let Format(p: ptr<Point>, f: Formatter) -> unit {
    write(f, "({}, {})", (*p).x, (*p).y)
  }
}

print("at {}\n", Point { x: 1, y: 2 });   // at (1, 2)
eprint("warning: {}\n", p);               // to stderr
panic("bad point {}", p);                  // in a panic's message
```

A `Formatter` is only handed on: to `write`, or to another instance's
`Format`. It carries the placeholder's digits, so an instance that hands it
to a float's prints the float with them, as a tuple's, an array's and an
`Option`'s do for their parts:

```sammine
struct Money { amount: f64, };

instance Format<Money> {
  let Format(m: ptr<Money>, f: Formatter) -> unit {
    write(f, "$");
    Format<f64>((*m).amount, f)
  }
}

print("{} {.2}\n", m, m);                 // $2.500000 $2.50
```

Printing only looks at a value, so an instance takes a view of it, `ptr<T>`,
and `print` lends the value: a `String` prints as `print("{}", s)` and is
still the program's afterwards (see Typeclasses).

### Panic

`panic("format", args...)` ends the program: it prints where it is and the
format, with its arguments as `print` prints them, to stderr, and exits with
status 1.

```sammine
let unwrap<T>(m: Maybe<T>) -> T {
  case m { Has(v) => v, Not => panic("unwrap of Not") }
}
let half(n: i32) -> i32 {
  if n % 2 == 0 { n / 2 } else { panic("{} is odd", n) }
}
// half(7) prints: file.mn:5:34: panic: 7 is odd
```

- **It gives no value**, so it fits where any is expected: a branch, a case's
  arm, a function's last value, what a `let` binds or takes apart (a name
  without an annotation is then `Never` too). Its value can't be used,
  as an argument, an operand or an element: that is an error, as it is for an
  `if` whose branches all panic or return.
- **The format is `print`'s**: a string literal, a `{}` for each argument,
  each printed through its type's `Format` instance, to stderr.
- **Nothing is dropped.** The program ends where it panics, as `exit()` ends
  it: values still owned, here or in the functions that called it, aren't
  dropped, and their `Drop` instances don't run. C's streams are flushed, so
  what was printed and written to files before it is kept. Its path joins no
  other, so a value moved where it doesn't panic is simply moved afterwards.

---

## 5. Operators

### Arithmetic

`+`, `-`, `*`, `/`, `%` (modulo). Work on integer and float types (except `%` which is integer-only):

```sammine
let a: i32 = 10;
let b: i32 = 3;
print("{}\n", a + b);    // 13
print("{}\n", a % b);    // 1
```

### Comparison

`==`, `!=`, `<`, `>`, `<=`, `>=`. Return `bool`:

```sammine
if a < b { print("less\n"); }
else { print("not less\n"); };
```

Each goes through its own typeclass, as `+` goes through `Add` (see [Builtin Typeclasses](#builtin-typeclasses)): `<` through `Less`, `>` through `Greater`, `<=` through `LessEqual`, `>=` through `GreaterEqual`, `==` through `Equal` and `!=` through `NotEqual`. Numbers and `char` can be compared every way, and `bool`, pointers, arrays of those and enums with `==` and `!=`. Two values of an enum are equal when they are the same variant with equal payloads, so `A(7) == B(7)` is false; an enum whose payloads can't be compared with `==` has no built-in `==` either. Any other type, such as a struct or tuple, needs an instance: comparing values without one is an error ("No instance of Equal<P>").

### Logical

`&&` (and), `||` (or). Short-circuit evaluation:

```sammine
if x > 0 && x < 100 {
  print("in range\n");
};
```

### Bitwise

`&` (and), `|` (or), `^` (xor), `<<` (left shift), `>>` (right shift). Work on integer types:

```sammine
let a: i32 = 10;
let b: i32 = 12;
print("{}\n", a & b);     // 8
print("{}\n", a | b);     // 14
print("{}\n", a ^ b);     // 6
print("{}\n", a << 2);    // 40
print("{}\n", a >> 2);    // 2
```

### Unary

`-x` negates a numeric value. Unsigned types cannot be negated (compile-time error):

```sammine
let x: i32 = 5;
print("{}\n", -x);        // -5
print("{}\n", -(-x));     // 5
```

### Conversions with `as`

`x as T` converts `x` to the type `T`:

```sammine
let letter_value(c: char) -> i32 {
  c as i32 - 'A' as i32 + 1          // 'C' gives 3
}

let average(total: i64, count: i32) -> f64 {
  total as f64 / count as f64
}
```

Between the number types, and between `char` and the integer types, the conversion is built in:

- **Widening an integer** keeps its value: a signed type, or `char`, extends its sign, and an unsigned type fills with zeros.
- **Narrowing an integer** keeps the low bits, like C: `5000000000i64 as i32` is `705032704`.
- **Float to integer** drops the fraction and saturates: `2.9 as i32` is `2`, NaN gives `0`, and a value out of range gives the type's min or max.
- **Integer to float, and float to float,** round to the nearest value the target holds.

A literal takes the type it is converted to when it can, so `5000000000 as i64` keeps its value. `as` binds tighter than every binary operator and looser than the prefix operators, so `a * b as i64` is `a * (b as i64)` and `-x as u32` is `(-x) as u32`.

`as` goes through the `Convert<From, To>` typeclass, so any other pair converts through a user's instance:

```sammine
struct Celsius { val: f64, };
struct Fahrenheit { val: f64, };

instance Convert<Celsius, Fahrenheit> {
  let Convert(x: Celsius) -> Fahrenheit {
    Fahrenheit { val: x.val * 9.0 / 5.0 + 32.0 }
  }
}

let f = Celsius { val: 100.0 } as Fahrenheit;   // 212
```

### Operator Precedence

From lowest to highest precedence. `as` binds tighter than all of these.

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
  print("x = {}\n", p.x);
  print("y = {}\n", p.y);
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
  print("{}\n", o.inner.value);    // 42
  return 0;
}
```

Types can be declared in any order, as functions can: `Outer` could come before `Inner`. The same holds for enums, type aliases, typeclasses and instances.

A type can't refer to itself, by value or through a pointer, directly or through another type. Both of these are errors:

```sammine
struct Node { v: i32, next: ptr<Node>, };   // 'Node' refers to itself
type T1 = T2;                               // Type alias 'T1' refers to
type T2 = T1;                               //   itself through 'T2'
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
  print("{}", b.value);       // 42
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

let rw: Flags = Read | Write;        // bitwise OR: value 3
let all: Flags = Read | Write | Exec; // value 7
```

### Case/Match Expressions

Pattern match on enums using `case`:

```sammine
type Color = Red | Green | Blue;

let color_name(c: Color) -> i32 {
  case c {
    Color::Red => print("red\n"),
    Color::Green => print("green\n"),
    Color::Blue => print("blue\n"),
  };
  return 0;
}
```

Destructure payload variants:

```sammine
type Shape = Circle(i32) | Rect(i32, i32) | Point;

let describe(s: Shape) -> i32 {
  case s {
    Shape::Circle(r) => print("circle r: {}\n", r),
    Shape::Rect(w, h) => print("rect w: {}, h: {}\n", w, h),
    Shape::Point => print("point\n"),
  };
  return 0;
}
```

Unqualified variant names work when unambiguous, both in patterns and constructors:

```sammine
let c: Color = Red;              // unqualified constructor (no Color:: needed)

case s {
  Circle(r) => print("circle r: {}\n", r),
  Rect(w, h) => print("rect w: {}, h: {}\n", w, h),
  Point => print("point\n"),
};
```

Wildcard `_` catches all remaining patterns:

```sammine
case c {
  Color::Red => { print("is red\n"); return 1; },
  _ => { print("not red\n"); return 0; },
}
```

### Generic Enums

Enums can be parameterized with type variables, just like structs:

```sammine
type Option<T> = Some(T) | None;

let main() -> i32 {
  let x: Option<i32> = Option<i32>::Some(42);
  case x {
    Some(v) => print("{}\n", v),
    None => print("none\n"),
  };
  return 0;
}
```

`Option<T>` is provided by the standard library and available in all programs without an import (see [Imports](#16-imports)).

Written without its type arguments, a constructor takes them from its payload, as a generic function call does. What the payload leaves open, as `None` leaves `T`, comes from where the value goes: a `let` annotation, a return type, a parameter, a field, or the other branch of an `if` or `case`. If nothing says, it's an error, and the type arguments are written out:

```sammine
let a = Some(3i64);                  // Option<i64>
let b = Some(7);                     // Option<i32>: the literal's default type
let c: Option<f64> = None;           // Option<f64>, from the annotation
let find(x: i64) -> Option<i64> {
  if x > 0i64 { Some(x) } else { None }   // None is an Option<i64>
}
let d = None;                        // error: nothing says what T is
let e = Option<f64>::None;           // written out
```
---

## 8. Generics

Generic functions are monomorphized at compile time -- a separate copy is generated for each concrete type used.

### Generic Functions

```sammine
let identity<T>(x: T) -> T { x }

let main() -> i32 {
  let a = identity(42);           // inferred: identity<i32>
  let b = identity(3.14);         // inferred: identity<f64>
  print("{} {.2}", a, b);   // 42 3.14
  return 0;
}
```

A literal argument takes its type from the other arguments when they decide
`T`, wherever it sits; only a literal nothing else decides defaults, as `42`
to `i32` above:

```sammine
let pick<T>(a: T, b: T) -> T { b }
let c = pick(2, 3000000000i64);   // inferred: pick<i64>, 2 as an i64
```

### Explicit Type Arguments

```sammine
let a = identity<i32>(42);
let b = identity<f64>(3.14);
```

### Generic Functions as Values

A generic function named with its types, without calling it, is a function
value: the copy for those types. It can be bound, passed, piped into, or
given to C, which takes it as a pointer to that copy's code:

```sammine
let apply(f: (i32) -> i32, x: i32) -> i32 { f(x) }
let apply_to<T>(f: (T) -> T, x: T) -> T { f(x) }

let f = identity<i32>;              // (i32) -> i32
apply(identity<i32>, 5);            // 5
let same<T>(x: T) -> T { apply_to(identity<T>, x) }  // the caller's T
```

A value has no arguments to infer the types from, so they are always given:
`apply(identity, 5)` is an error. A `>` followed by `,`, `)`, `]`, `}`, `;`
or `|>` closes type arguments; followed by anything else, it is a
comparison, as in `f(a < b, c > d)`.

### Generic Functions Calling Generic Functions

```sammine
let identity<T>(x: T) -> T { x }

let apply_identity<T>(x: T) -> T {
  identity<T>(x)
}

let main() -> i32 {
  print("{}\n", apply_identity<i32>(42));
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
  print("{}\n", mysize<i32>());    // 4
  print("{}\n", mysize<i64>());    // 8
  return 0;
}
```

### Qualified Call Syntax

Typeclass methods can be called with either syntax:

```sammine
mysize<i32>()                 // unqualified with type arg
MySize<i32>::mysize()         // qualified: ClassName<Type>::method()
```

A typeclass is a definition of its module, as a struct is, and each of its
methods is a name of that module too. Another module names them through it,
and declares an instance of the class the same way:

```sammine
import shapes;                           // export typeclass Area<T> { Area(x: T) -> f64; }

instance shapes::Area<Mine> { let Area(m: Mine) -> f64 { 1.0 } }

shapes::Area<Mine>(m)                    // the method named as its class
shapes::Area<Mine>::Area(m)              // the same, through the class
```

Two modules can each have a typeclass of the same name: they are two
classes. A module can't define a typeclass twice, or give two of its
typeclasses a method of the same name.

### Generic Instances

An instance can be written once for every type of a shape. Its type
parameters come right after `instance`:

```sammine
struct Pair<T> { a: T, b: T, };

instance<T> Add<Pair<T>> {
  let Add(x: Pair<T>, y: Pair<T>) -> Pair<T> {
    Pair<T> { a: x.a + y.a, b: x.b + y.b }
  }
}

let p = Pair<i64> { a: 1i64, b: 2i64 } + Pair<i64> { a: 10i64, b: 20i64 };
```

Like a generic function, the instance is copied for each set of types it is
used with, here `Add<Pair<i64>>`. Every parameter must appear in the
instance's types.

Two instances may not overlap: if some types would match both, which one
runs would be ambiguous, so it is an error. That holds for two generic
instances (`instance<T> Indexer<Box<T>, T>` and `instance<U> Indexer<Box<i32>, U>`
both match `Indexer<Box<i32>, i32>`) and for a generic and a concrete one
(`instance Indexer<Box<i32>, i32>` beside the first). The built-in instances
for numbers, `char` and pointers are the exception: they are always used for
those types.

An instance is written in the module of its class, or of one of the types it
is for. A program has one instance for a class and its types, so were any
module free to write it, two libraries could each write the same one, and no
program could use both. A type counts through pointers to it
(`Convert<ptr<Point>, i64>` is `Point`'s module's to write), and a type no module
defines, a number, a string, a tuple or an array, is no module's: only the
class's module writes an instance for those alone.

```sammine
import vec;

instance Format<vec::Vec<bool>> { ... }   // error: Format is the prelude's, Vec is vec's

struct Flags { bits: vec::Vec<bool>, };   // a type of this module's
instance Format<Flags> { ... }            // its instance to write
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
print("{}\n", Sized<i32>());     // 4
print("{}\n", Sized<i64>());     // 8
print("{}\n", Sized<f64>());     // 8
print("{}\n", Sized<bool>());    // 1
print("{}\n", Sized<char>());    // 1
```

**`Hash<T>`** -- user-defined hash function:

```sammine
struct Point { x: i32, y: i32, };

instance Hash<Point> {
  let Hash(val: ptr<Point>) -> u64 { Hash<i32>((*val).x) ^ Hash<i32>((*val).y) }
}

let h = Hash<Point>(Point { x: 1, y: 2 });
```

**A class that only looks at a value takes a view of it.** `Hash`, the
comparisons and `Format` are declared over `ptr<T>`: `Hash(val: ptr<T>)`,
`Less(a: ptr<T>, b: ptr<T>)`. The instance is still written for the type,
`Hash<Point>`, and what calls its method, an operator, a `{}` or the
method's name, gives the value and lends it, as `v[i]` lends `v` to
`Indexer`. So hashing, comparing and printing use nothing up, and a type that
owns memory, as `str::String`, has the same instances a number has: `a < b`,
`print("{}", a)`. A view is given as it is: inside an instance, `Less<T>(b, a)`
hands on the views it was given. `Add` and the other arithmetic classes take
their operands by value, as they make a new one of them.

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
let v3: Vec2 = v1 + v2;                 // uses Add<Vec2>
print("{} {}\n", v3.x, v3.y);     // 30 40
```

The number types have these instances built in: `Add`, `Sub`, `Mul` and `Div` for `i32`, `i64`, `u32`, `u64`, `f32` and `f64`, and `Mod` for the integer types. They can be called by name like any instance, e.g. `Add<i32>(3, 4)`, and can't be redefined.

**`Less<T>`**, **`Greater<T>`**, **`LessEqual<T>`**, **`GreaterEqual<T>`**, **`Equal<T>`**, **`NotEqual<T>`** -- comparison, used by `<`, `>`, `<=`, `>=`, `==` and `!=`. Each method takes views of two `T`s and returns `bool`. They are built in for the number types and `char`, and `Equal` and `NotEqual` also for `bool`, pointers, arrays of those, and enums whose payloads have them. Each operator only needs its own instance:

```sammine
struct Version { major: i32, minor: i32, };

instance Less<Version> {
  let Less(a: ptr<Version>, b: ptr<Version>) -> bool {
    (*a).major < (*b).major ||
        ((*a).major == (*b).major && (*a).minor < (*b).minor)
  }
}

let older = Version { major: 1, minor: 4 } < Version { major: 2, minor: 1 };  // true
```

**`Convert<From, To>`** -- conversion, used by `as` (see [Conversions with `as`](#conversions-with-as)). It is built in between the number types, and between `char` and the integer types, so `Convert<char, i32>('a')` is `'a' as i32`.

**`Indexer<Collection, Element>`** -- custom indexing. `x[i]`, for an `x` that
is neither an array nor a pointer, calls the one `Indexer` instance for `x`'s
type. The collection is lent as a `ptr`, so indexing doesn't use it up, and the
index is an `i64`:

```sammine
struct IntList { a: i32, b: i32, c: i32, };

instance Indexer<IntList, i32> {
  let Indexer(coll: ptr<IntList>, idx: i64) -> i32 {
    if idx == 0i64 { (*coll).a }
    else if idx == 1i64 { (*coll).b }
    else { (*coll).c }
  }
}

let list = IntList { a: 10, b: 20, c: 30 };
let x = list[1];                               // 20
let y = Indexer<IntList, i32>(&list, 2i64);    // 30, the same call by name
```

`x[i]` only reads: `x[i] = v` is an error. The element comes back by value
while the collection still holds it, so an element type that owns memory, like
`'ptr<i32>`, can't be indexed this way. Two instances for one collection type
with different element types make `x[i]` ambiguous.

---

## 10. Tuples

Tuple literals group multiple values. Destructure with pattern matching on `let`:

```sammine
let t = (42, true);          // type: (i32, bool)
let (a, b) = t;              // destructuring
print("{}\n", a);      // 42
print("{}\n", b);      // 1 (true printed as integer)
```

---

## 11. Arrays

Fixed-size, stack-allocated arrays with compile-time known length.

### Declaration and Indexing

```sammine
let a: [i32; 3] = [10, 20, 30];
print("{}\n", a[0]);           // 10
print("{}\n", a[1]);           // 20
print("{}\n", a[2]);           // 30
```

### Array Length

```sammine
let a: [i32; 5] = [1, 2, 3, 4, 5];
print("{}\n", len(a));         // 5
```

Array elements cannot be written to.

### Bounds Checking

Out-of-bounds access with a constant index is caught at compile time:

```sammine
let a: [i32; 3] = [10, 20, 30];
print("{}\n", a[5]);
// Error: Array index out of bounds: index 5 on array of size 3
```

Any other index is checked when the program runs. Out of bounds, the program
prints `index 7 out of bounds for an array of length 3` to stderr and exits
with status 1.

---

## 12. Pointers and Memory

sammine-lang has two pointer kinds: **linear** (`'ptr<T>`) for heap-allocated memory that must be freed, and **non-linear** (`ptr<T>`) for stack references.

### Heap Allocation (Linear Pointers)

`alloc<T>(count)` allocates memory on the heap and returns a linear `'ptr<T>`. A linear pointer is used at most once -- by `drop()`, a move, or a return -- and dropped where it goes out of use if nothing took it:

```sammine
let p: 'ptr<i32> = alloc<i32>(1);    // allocate 1 element
*p = 42;                              // write through pointer
let y: i32 = *p;                      // dereference (read)
drop(p);                              // deallocate (consumes p)
print("{}\n", y);               // 42
```

Assignment only writes through a pointer: `*p`, `p[i]`, and any field or
element reached from them, such as `(*p).x`, `p[i].x` or `(*p)[i][j]`. Variables,
array values and struct values can't be assigned to.

A value that owns memory must have an owner: a temporary nothing takes,
such as `make_ptr();` as a statement, is an error ("Temporary owning memory
must not be discarded (bind it with 'let' first)"). A value bound with `let`,
or a parameter, that goes out of use with nothing taking it is dropped
there: at a return, at the end of the arm or loop turn that made
it, or, where one branch moved it and another didn't, on the other branch.
Dropping a `'ptr<T>` drops what it points at, then frees it; a `'buf<T>` is
freed; a struct or tuple drops field by field. Using a value after it moved
is still an error:

```sammine
let p = alloc<i32>(1);
*p = 42;
let q = p;           // p moves to q
drop(p);             // Error: Cannot use 'p' — it was already moved previously
```

A type says what its end does with a `Drop` instance, which takes the value
and must use up what it owns; nothing else is dropped after it. Such a
value drops whole: moving a field out of one is an error, but in an
unsafe function, as the type's own code does. `Vec<T>`'s
drops each value it holds, then its buffer; `io::File`'s closes the file:

```sammine
instance Drop<File> {
  let unsafe Drop(f: File) -> unit { sammine_io_close(f.handle) }
}
```

### Address-Of (Non-Linear Pointers)

`&x` takes the address of a stack variable and returns a non-linear `ptr<T>`:

```sammine
let x: i32 = 42;
let p: ptr<i32> = &x;
let y: i32 = *p;                     // dereference
print("{}\n", y);              // 42
```

### What a Non-Linear Pointer Can Do

A `ptr<T>` is a view: you can look through it, but you don't own what it points
at. The compiler enforces that:

- **Read-only.** Writing through a `ptr<T>` (`*p = v`, `p[i] = v`, `(*p).f = v`)
  is an error. Only an owned `'ptr<T>` can be written.
- **Parameters and locals only.** A `ptr<T>` can't be returned, stored in a
  struct, tuple, array or enum, or held by a `'ptr<T>`. A view of a view, such
  as `argv: ptr<ptr<char>>`, is allowed.
- **Never dropped.** Only the owner drops (and so frees) memory.

An owned pointer can be **lent** to a function that takes a `ptr<T>`. The caller
keeps ownership and still has to drop it:

```sammine
let look(p: ptr<i32>) -> i32 { *p }

let main() -> i32 {
  let b = alloc<i32>(1);
  *b = 7;
  let v = look(b);      // b is lent, not moved
  drop(b);              // main still owns b
  return 0;
}
```

Any value is lent the same way: given where a `ptr<T>` is taken, a `T` is
lent, a view of it for the call, and stays the caller's. So a function that
only looks at something takes a view of it, and is called with the thing
itself, a binding's value or one made in place:

```sammine
let size(s: ptr<str::String>) -> i64 { str::length(s) }

let name = str::new("ada");
size(name);                  // name is lent, and still the caller's
size(str::new("grace"));     // lent where it is made, then dropped
map::get(days, name);        // both are lent
```

`&x` still makes a view to keep in a binding. Lending only happens at a call:
`let q: ptr<i32> = b;` is a type mismatch, and taking the address of owned
memory (`&*b`, `&b[0]`) or of a value no binding names (`&make()`) is an
error, since the view could outlive what it views.

### Pointer Indexing

Buffers can be indexed like arrays, but only inside a function declared
`let unsafe`. A `'ptr<T>` holds one value, reached as `*p`, and isn't indexed;
a `'buf<T>` holds many, and a view of either can be:

```sammine
let unsafe main() -> i32 {
  let p: 'buf<i32> = alloc_buf<i32>(3);
  p[0] = 10;
  p[1] = 20;
  p[2] = 42;
  print("{}\n", p[0]);         // 10
  print("{}\n", p[2]);         // 42
  drop(p);
  return 0;
}
```

The compiler checks neither which element `p[i]` names nor whether it is in
bounds, so the linearity of owned elements reached through `p[i]` is not
checked either. `let unsafe` marks the functions where that is up to the
author: when a program crashes, look at its unsafe functions first. This holds
for every buffer and view, a view of a string's chars too
(`("hi" as ptr<char>)[0]`).
Fixed arrays are bounds-checked and can be indexed anywhere.

### Buffers

A `'ptr<T>` owns one value. Memory holding many, as `Vec<T>` and `String`
keep, is a buffer, `'buf<T>`: `alloc_buf<T>(n)` allocates room for `n` values.
A buffer is owned like a `'ptr<T>`, moved, lent to a `ptr<T>` parameter and
dropped the same way, but it has no single value to dereference: `*b` is an
error, and its values are reached by index, in an unsafe function. How many of
them hold a value is up to the code that owns the buffer, so the compiler
doesn't follow them: dropping a buffer frees the memory and nothing in it.

```sammine
reuse strlen(s: ptr<char>) -> i64;

let unsafe main() -> i32 {
  let s = alloc_buf<char>(3);
  s[0] = 'h'; s[1] = 'i'; s[2] = '\0';
  print("{}\n", strlen(s));   // 2: s is lent
  drop(s);
  return 0;
}
```

`'ptr<T>` and `'buf<T>` are different types.

An unsafe function is called like any other function. Callers need no marker,
so `vec::push` is unsafe inside but used from safe code:

```sammine
let unsafe put(p: 'buf<i64>, i: i64, v: i64) -> 'buf<i64> {
  p[i] = v;
  p
}

let main() -> i32 {
  let p = put(alloc_buf<i64>(1), 0i64, 7i64);
  drop(p);
  return 0;
}
```

`export let unsafe f(...)` exports an unsafe function.

### Ownership Transfer (Move)

Assigning a linear pointer to a new variable moves ownership:

```sammine
let p = alloc<i32>(1);
*p = 55;
let q = p;            // p is consumed, q now owns the memory
let v: i32 = *q;
drop(q);              // must drop q, not p
```

### Returning Linear Pointers

Functions can transfer ownership by returning a linear pointer:

```sammine
let make_ptr() -> 'ptr<i32> {
  let p = alloc<i32>(1);
  *p = 123;
  return p;           // ownership transferred to caller
}

let main() -> i32 {
  let p = make_ptr();
  let v: i32 = *p;
  drop(p);            // caller must drop
  print("{}\n", v);
  return 0;
}
```

---

## 13. Pipe Operator

The pipe operator `|>` passes the left-hand value as the first argument to the right-hand function:

```sammine
let square(x: i32) -> i32 { x * x }

print("{}\n", 5 |> square);     // square(5) = 25
```

With additional arguments:

```sammine
let add(a: i32, b: i32) -> i32 { a + b }

print("{}\n", 5 |> add(3));     // add(5, 3) = 8
```

Pipes chain left-to-right:

```sammine
let square(x: i32) -> i32 { x * x }
let double(x: i32) -> i32 { x + x }
let add(a: i32, b: i32) -> i32 { a + b }

let result = 5 |> square |> double |> add(1);
print("{}\n", result);           // ((5*5)*2)+1 = 51
```

---

## 14. Partial Application

Calling a function with fewer arguments than it expects creates a closure:

```sammine
let add(a: i32, b: i32) -> i32 { a + b }

let add5: (i32) -> i32 = add(5);     // partially apply first arg
print("{}\n", add5(3));         // 8
print("{}\n", add5(7));         // 12
```

Multi-argument partial application:

```sammine
let add3(a: i32, b: i32, c: i32) -> i32 { a + b + c }

let add1_2: (i32) -> i32 = add3(1, 2);
print("{}\n", add1_2(7));       // 10
```

Partial application works with higher-order functions:

```sammine
let apply(f: (i32) -> i32, x: i32) -> i32 { f(x) }

print("{}\n", apply(add(10), 32));    // 42
```

A closure carries what it binds, by value, so it can go anywhere a value
can: returned, stored in a struct, an array or an enum, put behind an owned
pointer. Nothing is allocated for it:

```sammine
let make(n: i32) -> (i32) -> i32 { add(n) }

let add5 = make(5);
print("{}\n", add5(3));         // 8
```

The compiler works out which closures each function value can be, and a
call through one whose closures it knows is a direct call, which the
optimizer can inline. `twice(add(1))`, for `twice(f: (i32) -> i32, x: i32)`,
binds an `(i32) -> i32` and is one, and can still be returned: it carries
`add(1)`'s binding, which is not itself.

Two kinds of closure stay in the call that made them, as a view (`ptr<T>`)
does: one that bound a view, and one that can hold itself through what it
binds, such as `nest(twice(f), n - 1)` passing `twice(f)` on as the `f` of
the next call: it would have to carry itself without end, so it keeps what
it binds in the frame of the call that made it. Either can be called and
passed to a function that only calls it; returning it, storing it, or
passing it to a function that keeps it is an error.

---

## 15. Higher-Order Functions

Functions are first-class values. Function types are written as `(ParamTypes) -> ReturnType`:

### Functions as Values

```sammine
let inc(x: i32) -> i32 { x + 1 }

let f: (i32) -> i32 = inc;
print("{}\n", f(41));           // 42
```

### Functions as Parameters

```sammine
let square(x: i32) -> i32 { x * x }

let apply(f: (i32) -> i32, x: i32) -> i32 { f(x) }

print("{}\n", apply(square, 5));    // 25
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
import std;                            // import the std module
print("hello {}\n", 42);        // qualified access
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

Use `export` to make definitions visible to importers. `export` works with functions, structs, enums, typeclasses, typeclass instances, and C bindings:

```sammine
// In math.mn:
export let add(a: i32, b: i32) -> i32 { a + b }
export struct Point { x: i32, y: i32, };
export type Color = Red | Green | Blue;
export typeclass Norm<T> { Norm(x: T) -> i32; }
export instance Norm<Point> { let Norm(p: Point) -> i32 { p.x + p.y } }
export reuse printf(fmt: string, ...) -> i32;
```

An exported function or instance is compiled in its own module, and an importer calls that compiled code, so the module must be compiled first. Its body can use the module's definitions that aren't exported.

A function, instance, type or typeclass that isn't exported is private to its module: other modules can't name or use it ("'vec::grow_buf' is private to module 'vec'"). An exported function or struct can still use the module's private types, in its body, signature or fields, and their values can reach other modules, which just can't write their names.

A generic function is compiled in each module that uses it, for the types it is used with, so its source comes along with the import. It still has to be exported to be called from another module. It counts as its module's own code wherever it is compiled, so an exported generic function can use its module's private functions and instances, as `vec::push` calls `vec`'s own `grow_buf`.

### Standard Library Modules

- **`definitions`** (the prelude) -- imported by every module, library or program, without writing `import`, and its names are used without a prefix. A module's own definition of the same name hides the prelude's in that module's code. Provides builtin typeclasses (`Sized`, `Add`, `Sub`, `Mul`, `Div`, `Mod`, `Less`, `Greater`, `LessEqual`, `GreaterEqual`, `Equal`, `NotEqual`, `Convert`, `Hash`, `Format`, `Indexer`, `Iterator`) with instances for primitive types, `Option<T>`, and `Range<T>`. `Hash` has instances for the integer types, `char` and `bool`, which mix every bit of the value into the hash (splitmix64's finalizer), so nearby keys land far apart in a table.
- **`std`** -- provides C's `printf`, for an unsafe function; `print` is built in
- **`io`** -- provides `printf` (C's, for an unsafe function), `print_i32`, `print_i64`, `print_f64`, `print_char`, `print_bool`, `read_line`, `read_i32`, `read_i64`, `read_f64`, file I/O (`open`, `close`, `file_read_line`, `file_write`, `read_file`, `write_file`, `append_file`). `open` gives an `io::File`, which `close` ends. `read_line()`, `read_file(path)` and `file_read_line(f)` return an `Option<str::String>`: the text, a `String` the caller owns, which prints as `print("{}", &text)`, or `None` at end of input or when the file cannot be read. The write functions take the chars and how many, so a literal or a `String`'s `data` and `length`. `file_read_line` and `file_write` hand the file back, as `(f, result)` and `f`.
- **`vec`** -- provides `Vec<T>` dynamic array (see below)
- **`map`** -- provides `Map<K, V>`, a hash table (see below)
- **`set`** -- provides `Set<T>`, a set of values over `Map<T, bool>` (see below)
- **`str`** -- provides `String` type with `new` and `delete`; `str::push(s, c)` and `str::append(s, "...")` give `s` with a `char` or a C string after it, growing it as `Vec`'s push does; `str::length(&s)`; `str::get(&s, i)` is `s[i]` giving `Option<char>`, `None` out of bounds; `str::eq(&a, &b)` compares what two strings hold; `s[i]` is byte `i` of `s` as a `char`, for an `i64` index in `0..s.length` (the terminator isn't among them), and any other panics, as `v[i]` does for a `Vec`, through `Indexer<String, char>`. It also orders strings byte by byte as C's `strcmp` does: `str::compare(&a, &b)` gives a negative, zero or positive `i32`; strings compare by what they hold, `a == b` and `a < b` (the comparison classes' instances on `String`: a comparison is lent the strings and uses neither up), hash as their text does, and print, `print("{}", s)`. `==` on views of them still compares addresses, as for any pointer

---

## 17. Vec\<T\>

`Vec<T>` is a generic, auto-growing dynamic array from the standard library.

```sammine
import std;
import vec;

let main() -> i32 {
  let v0 = vec::new<i32>(4);             // create with initial capacity 4
  let v1 = vec::push<i32>(v0, 10);       // push returns new Vec
  let v2 = vec::push<i32>(v1, 20);
  let v3 = vec::push<i32>(v2, 30);
  print("{} {} {}", v3[0], v3[1], v3[2]);  // 10 20 30
  vec::delete<i32>(v3);                   // must free (linear data pointer)
  return 0;
}
```

### API

| Function | Signature | Description |
|----------|-----------|-------------|
| `vec::new<T>(cap)` | `(i64) -> Vec<T>` | Create a new Vec with given capacity |
| `vec::push<T>(v, val)` | `(Vec<T>, T) -> Vec<T>` | Append an element, auto-grows if full |
| `vec::pop<T>(v)` | `(Vec<T>) -> (Vec<T>, Option<T>)` | Take the last element off: `None` if `v` is empty |
| `vec::set<T>(v, i, val)` | `(Vec<T>, i64, T) -> Vec<T>` | Replace element `i`, dropping the one it held; panics outside `0..length`, as `v[i]` |
| `vec::get<T>(&v, i)` | `(ptr<Vec<T>>, i64) -> Option<T>` | Element `i`, or `None` outside `0..length`: `v[i]` that doesn't panic. For an element type that owns nothing |
| `vec::length<T>(&v)` | `(ptr<Vec<T>>) -> i64` | How many elements `v` holds |
| `vec::delete<T>(v)` | `(Vec<T>) -> unit` | Free the underlying memory |
| `v[i]` | `Indexer<Vec<T>, T>` | Element `i`, for an element type that owns nothing. An `i` outside `0..v.length` panics: `…vec.mn:12:7: panic: index 2 out of bounds for a Vec of length 2` on stderr, status 1 |

### Vec\<T\> Structure

```sammine
struct Vec<T> {
  data: 'buf<T>,     // owned buffer on the heap
  length: i64,       // current number of elements
  cap: i64,          // allocated capacity
};
```

`push` is a functional-style operation: it returns a new `Vec<T>` value. When capacity is exceeded, the buffer is doubled automatically; one made with capacity 0 gets room for 4. Each intermediate `Vec` value is consumed (moved) by the next `push` call, so only the final `Vec` needs an explicit `delete`.

### Auto-Growth Example

```sammine
import vec;

let v0 = vec::new<i32>(2);     // capacity 2
let v1 = vec::push<i32>(v0, 0);
let v2 = vec::push<i32>(v1, 1);
let v3 = vec::push<i32>(v2, 2);  // triggers growth to capacity 4
// ... keep pushing, grows as needed
vec::delete<i32>(v3);
```

---

## 18. Map\<K, V\> and Set\<T\>

`map::Map<K, V>` is a hash table from keys to values; `set::Set<T>` holds each
value once. Keys and set values need a `Hash` instance, as the integers,
`char`, `bool`, `string` and `str::String` have (a key type without one is an
error, "No instance of Hash<...>"), and `==`. Keys and values may own memory:
the map owns what is put in it, and drops it with itself. As with `Vec`, what
changes a map takes it and gives it back; what reads one is lent it, and the
key it looks up.

```sammine
import std;
import map;

let main() -> i32 {
  let m = map::insert<i32, i32>(map::new<i32, i32>(), 1, 10);
  let m = map::insert<i32, i32>(m, 1, 11);           // replaces 10
  let v = case map::get<i32, i32>(&m, 1) { Some(v) => v, None => 0 };
  print("{}\n", v);                            // 11
  let (m, old) = map::remove<i32, i32>(m, 1);        // old: Some(11)
  map::delete<i32, i32>(m);
  0
}
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `map::new<K, V>()` | `() -> Map<K, V>` | An empty map |
| `map::insert<K, V>(m, k, v)` | `(Map<K, V>, K, V) -> Map<K, V>` | Map `k` to `v`: the map takes both. If it maps `k` already, the value it had is dropped, and `k` too |
| `map::get<K, V>(m, k)` | `(ptr<Map<K, V>>, ptr<K>) -> Option<V>` | The value `k` maps to, or `None`: `m` and `k` are lent. For a value type that owns nothing |
| `map::contains<K, V>(m, k)` | `(ptr<Map<K, V>>, ptr<K>) -> bool` | Whether `k` maps to a value |
| `map::remove<K, V>(m, k)` | `(Map<K, V>, ptr<K>) -> (Map<K, V>, Option<V>)` | Take `k` out, handing its value back and dropping the key the map held |
| `map::length<K, V>(m)` | `(ptr<Map<K, V>>) -> i64` | How many keys `m` maps |
| `map::delete<K, V>(m)` | `(Map<K, V>) -> unit` | Drop the map and the keys and values it holds |
| `set::new<T>()`, `set::insert<T>(s, x)`, `set::contains<T>(s, x)`, `set::remove<T>(s, x)`, `set::length<T>(s)`, `set::delete<T>(s)` | | The same for a set; `remove` gives `(Set<T>, bool)`, whether `x` was in it |

A map is open-addressed with linear probing: a key sits in the first free
slot from where it hashes to. It keeps at most 3/4 of its slots used,
doubling when most of those hold keys and rebuilding at its size when most
are removed ones, so a lookup walks a few slots.
