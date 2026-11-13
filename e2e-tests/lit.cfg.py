
import lit.formats
import os
import shutil

# Resolve paths from CMake params, falling back to relative paths for manual runs.
bin_dir = lit_config.params.get('bin_dir', os.path.abspath("../bin"))
build_dir = lit_config.params.get('build_dir', os.getcwd())

sammine_exe = os.path.join(bin_dir, "sammine")
sammine_check = os.path.join(bin_dir, "SammineCheck")
sammine_opt = os.path.join(bin_dir, "sammine-opt")
# C compiler for tests that link C code, e.g. to check the C calling convention.
c_compiler = lit_config.params.get('cc', shutil.which('cc') or 'cc')

config.name = "Sammine Lang end-to-end testsuite"

# Feature detection: CUDA available if ptxas is on PATH
if shutil.which("ptxas"):
    config.available_features.add("cuda")

# %asan runs what follows it with ASan's runtime preloaded: its allocator
# reports a leak at exit, and a free twice or of what malloc didn't give, in
# code nothing instrumented. No build of the compiler has a sanitizer; a
# test opts in on its own RUN line. On a compiled program, it checks the
# drops. On sammine itself, add ASAN_OPTIONS=detect_leaks=0: the linker it
# runs inherits the preload, and has leaks of its own.
import subprocess
asan_runtime = None
if shutil.which("clang"):
    path = subprocess.run(["clang", "-print-file-name=libclang_rt.asan-x86_64.so"],
                          capture_output=True, text=True).stdout.strip()
    if os.path.isabs(path) and os.path.exists(path):
        asan_runtime = path
if asan_runtime:
    config.available_features.add("asan")
    config.substitutions.append(('%asan', 'env LD_PRELOAD=' + asan_runtime))

# Direct test output (Output/ dirs, %t temps) into the build tree so the
# source tree stays clean.  Without this, lit defaults to placing Output/
# next to the .mn source files.
config.test_exec_root = build_dir

# Per-test output directory to avoid parallel races.
# lit's %t is a unique temp path per test; we use %t.d as a directory.
# ShTest preamble_commands run before every RUN line, ensuring the dir exists.
config.substitutions.append(('%sammine_opt', sammine_opt))
config.substitutions.append(('%sammine_jit', sammine_exe + ' --jit'))
config.substitutions.append(('%sammine', sammine_exe))
config.substitutions.append(('%check', sammine_check))
config.substitutions.append(('%cc', c_compiler))
config.substitutions.append(('%dir', '$(dirname %s)'))
config.substitutions.append(('%full', '%s'))
config.substitutions.append(('%base', '$(basename %s .mn)'))
config.substitutions.append(('%O', '-O %t.d'))
config.substitutions.append(('%I', '-I %t.d'))
config.substitutions.append(('%T', '%t.d'))
config.test_format = lit.formats.ShTest(execute_external=True,
                                        preamble_commands=['mkdir -p %t.d'])
config.suffixes = ['.c', '.mn', '.mlir']
config.excludes = ['Inputs']
