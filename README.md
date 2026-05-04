# sammine-lang

> A compiled programming language built with C++23 and LLVM/MLIR. Features: type inference, generics, partial application, pipe operators, linear types, structs, enums, typeclasses, and more. Follow along via Jasmine's [blog](https://badumbatish.github.io/blog).

![](https://github.com/badumbatish/sammine-lang/blob/main/img.png)
How I pictured sammine-lang in my head

## Demo

See e2e-tests/compilables/misc/readme_demo.mn for a demo of the language
## Dev

### Architecture

sammine relies on LLVM and StableHLO for its back end. We'll use them as git submodules pinned
to a specific compatible version. 

We'll use nix and just file to determinatize and automate these steps.

### Steps

Run `nix develop` to enter the development workspace.

Run `just` for all the sensible commands, but the following is what you should do

1. `just init`: clone and initialize llvm-project and stablehlo.

2. `just llvm`: configure llvm and build it.

3. `just configure`: configure the main build.

4. `just build` (or `just b`): build the main build under debug.

5. `just test`: test all the tests.

Other commands include:
- `just clean`: clean (rm -rf) the build dir of sammine
- `just run`: run the compiler on a file (you can include extra flags).



Run
```bash
rm -rf build
```
to erase the build folder (similar to make clean).

## Compiler CLI

```
  --help
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

### TODO

Optional flags:
- `-DSAMMINE_SANITIZE=ON` — Enable ASan + UBSan
- `-DSAMMINE_TRACY=ON` — Enable [Tracy](https://github.com/wolfpld/tracy) profiler instrumentation (uses `TRACY_ON_DEMAND`, no overhead until the Tracy GUI connects)

