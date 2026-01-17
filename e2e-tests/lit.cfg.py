
import lit.formats
import os
# config.test_source_root = os.path.dirname(__file__)

# Paths are relative to build/e2e-tests/ (where cmake runs lit from)
sammine_exe = os.path.abspath("../bin/sammine")
sammine_check = os.path.abspath("../bin/SammineCheck")

config.name = "Sammine Lang end-to-end testsuite"
config.test_format = lit.formats.ShTest(True)

config.substitutions.append(('%sammine', sammine_exe))
config.substitutions.append(('%check', sammine_check))
config.substitutions.append(('%basename_s', '$(basename %s .mn)'))
config.suffixes = ['.c', '.mn']
