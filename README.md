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
./build/src/sammine --help

Usage: sammine [--help] [--version] [[--file VAR]|[--str VAR]] [--llvm-ir] [--diagnostics]

Optional arguments:
  -h, --help      shows help message and exits
  -v, --version   prints version information and exits
  -f, --file      An input file for compiler to scan over.
  -s, --str       An input string for compiler to scan over.

diagnostics (detailed usage):
   --llvm-ir      sammine compiler spits out LLVM-IR to stdout
   --diagnostics  sammine compiler spits out diagnostics for sammine-lang developers
```

Failed scoping:

```
./build/src/sammine -f unit-tests/artifacts/invalid_grammar.txt

    |At unit-tests/artifacts/invalid_grammar.txt:4:4
   2|fn f(x : f64) {
   3|    # Use `let` keyword
   4|    let x = 0;
    |    ^^^^^^^^^
    |    [SCOPE1]: The name `x` has been introduced before
   5|    3 + 1;
   6|    x + 2;
----|
    |At unit-tests/artifacts/invalid_grammar.txt:2:5
   1|# this is a function
   2|fn f(x : f64) {
    |     ^^^^^^^
    |     [SCOPE1]: Most recently defined `x` is here
   3|    # Use `let` keyword
   4|    let x = 0;

# Did something seems wrong? Report it via [https://codeberg.org/badumbatish/sammine-lang/issues]
# Give us a screenshot of the error as well as your contextual source code
```
