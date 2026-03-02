
import lit.formats
import os
# config.test_source_root = os.path.dirname(__file__)

# Paths are relative to build/e2e-tests/ (where cmake runs lit from)
sammine_exe = os.path.abspath("../bin/sammine")
sammine_check = os.path.abspath("../bin/SammineCheck")

config.name = "Sammine Lang end-to-end testsuite"
config.test_format = lit.formats.ShTest(True)

# Built-in lit substitutions used below:
#   %s  — full path to the current test file
#   %T  — temporary directory for the test suite (shared across tests)
config.substitutions.append(('%sammine', sammine_exe))
config.substitutions.append(('%check', sammine_check))
config.substitutions.append(('%dir', '$(dirname %s)'))
config.substitutions.append(('%full', '%s'))
config.substitutions.append(('%base', '$(basename %s .mn)'))
config.substitutions.append(('%O', '-O %T'))   # output artifacts to temp dir
config.substitutions.append(('%I', '-I %T'))   # search temp dir for imports
config.suffixes = ['.c', '.mn']
config.excludes = ['Inputs']
