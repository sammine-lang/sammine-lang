<h1>sammine-lang</h1>

> Jasmine's education front end compiler front end via her [blog](https://badumbatish.github.io/blog)

![](https://github.com/badumbatish/sammine-lang/blob/main/img.png)
How I pictured sammine-lang in my head


<h2>Dev</h2>

<h3>MacOS Configurations</h3>

Install llvm & catch2

```bash
brew install llvm catch2
```
Run
```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug [-DSAMMINE_TEST=ON/OFF] [-DCMAKE_LINKER_TYPE=MOLD]
```

for configuration. We need [llvm](https://github.com/Shuriken-Group/setup_llvm_tools),[FileCheck](https://pypi.org/project/filecheck/), and [lit](https://pypi.org/project/lit/).


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

<h2>Simple Demo</h2>

Compiler help

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

Fibonacci — `e2e-tests/compilables/func/fib.mn`:

```sammine
# RUN: %sammine --file %full && ./%base.exe | %check %full
# CHECK: 34
# CHECK-NEXT: 34
let fib_v1(x: i32) -> i32 {
  if x == 0 || x == 1 {
    return x;
  } else {
    return fib_v1(x-1) + fib_v1(x-2);
  }
}

let fib_v2(x: i32) -> i32 {
  if x == 0 || x == 1 {
    x
  } else {
    fib_v2(x-1) + fib_v2(x-2)
  }
}

let main() -> i32 {
  printf("%d\n", fib_v1(9));
  printf("%d\n", fib_v2(9));
  return 0;
}
```

Pointers with `alloc`/`free` — `e2e-tests/compilables/ptr/alloc_basic.mn`:

```sammine
# RUN: %sammine --file %full && ./%base.exe | %check %full
# CHECK: 42
let main() -> i32 {
  let p : ptr<i32> = alloc(42);
  let y : i32 = *p;
  free(p);
  printf("%d\n", y);
  return 0;
}
```

Arrays — `e2e-tests/compilables/array/arr_basic.mn`:

```sammine
# RUN: %sammine --file %full && ./%base.exe | %check %full
# CHECK: 10
# CHECK: 20
# CHECK: 30
let main() -> i32 {
  let a : [i32;3] = [10, 20, 30];
  printf("%d\n", a[0]);
  printf("%d\n", a[1]);
  printf("%d\n", a[2]);
  return 0;
}
```

Immutable-by-default variables with `let mut` — `e2e-tests/compilables/misc/mut_reassign.mn`:

```sammine
# RUN: %sammine --file %full && ./%base.exe | %check %full
# CHECK: 10

let main() -> i32 {
  let mut x: i32 = 5;
  x = 10;
  printf("%d\n", x);
  return 0;
}
```
