
import lit.formats
import os
import shutil

# Resolve paths from CMake params, falling back to relative paths for manual runs.
bin_dir = lit_config.params.get('bin_dir', os.path.abspath("../bin"))
build_dir = lit_config.params.get('build_dir', os.getcwd())

sammine_exe = os.path.join(bin_dir, "sammine")
sammine_check = os.path.join(bin_dir, "SammineCheck")

config.name = "Sammine Lang end-to-end testsuite"

# Feature detection: CUDA available if ptxas is on PATH
if shutil.which("ptxas"):
    config.available_features.add("cuda")

# Direct test output (Output/ dirs, %t temps) into the build tree so the
# source tree stays clean.  Without this, lit defaults to placing Output/
# next to the .mn source files.
config.test_exec_root = build_dir

# Per-test output directory to avoid parallel races.
# lit's %t is a unique temp path per test; we use %t.d as a directory.
# ShTest preamble_commands run before every RUN line, ensuring the dir exists.
config.substitutions.append(('%sammine_jit', sammine_exe + ' --jit'))
config.substitutions.append(('%sammine', sammine_exe))
config.substitutions.append(('%check', sammine_check))
config.substitutions.append(('%dir', '$(dirname %s)'))
config.substitutions.append(('%full', '%s'))
config.substitutions.append(('%base', '$(basename %s .mn)'))
config.substitutions.append(('%O', '-O %t.d'))
config.substitutions.append(('%I', '-I %t.d'))
config.substitutions.append(('%T', '%t.d'))
config.test_format = lit.formats.ShTest(execute_external=True,
                                        preamble_commands=['mkdir -p %t.d'])
config.suffixes = ['.c', '.mn']
config.excludes = ['Inputs']
