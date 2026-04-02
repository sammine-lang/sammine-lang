
import lit.formats
import os
import shutil

# Paths are relative to build/e2e-tests/ (where cmake runs lit from)
sammine_exe = os.path.abspath("../bin/sammine")
sammine_check = os.path.abspath("../bin/SammineCheck")

config.name = "Sammine Lang end-to-end testsuite"
config.test_format = lit.formats.ShTest(True)

# Feature detection: CUDA available if ptxas is on PATH
if shutil.which("ptxas"):
    config.available_features.add("cuda")

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
