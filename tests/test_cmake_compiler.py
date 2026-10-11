#!/usr/bin/env python3
"""Exercise compiler discovery without depending on the host's installed tools."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--cmake', required=True)
parser.add_argument('--selector', required=True)
args, unittest_args = parser.parse_known_args()


class CompilerSelection(unittest.TestCase):
    def select(self, tools, languages='C CXX', setup='', env_overrides=None):
        with tempfile.TemporaryDirectory(prefix='cmake compiler ') as temp:
            root = Path(temp)
            binaries = root / 'tools'
            binaries.mkdir()
            for name in tools:
                executable = binaries / (name + ('.exe' if os.name == 'nt' else ''))
                executable.write_text('#!/bin/sh\nexit 0\n')
                executable.chmod(0o755)
            script = root / 'select.cmake'
            output = root / 'compilers.txt'
            script.write_text(f'''
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH FALSE)
set(CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH FALSE)
set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH FALSE)
set(CMAKE_PROGRAM_PATH [[{binaries.as_posix()}]])
include([[{Path(args.selector).resolve().as_posix()}]])
{setup}
m0boy_select_host_compiler({languages})
file(WRITE [[{output.as_posix()}]] "${{CMAKE_C_COMPILER}}\n${{CMAKE_CXX_COMPILER}}\n")
''')
            env = {k: v for k, v in os.environ.items() if k not in ('CC', 'CXX', 'CMAKE_TOOLCHAIN_FILE')}
            env.update(env_overrides or {})
            subprocess.run([args.cmake, '-P', str(script)], env=env, check=True, capture_output=True, text=True)
            return [Path(line).stem if line else '' for line in output.read_text().splitlines()]

    def test_clang_pair_preferred(self):
        self.assertEqual(self.select(['clang', 'clang++', 'gcc', 'g++']), ['clang', 'clang++'])

    def test_gcc_fallback(self):
        self.assertEqual(self.select(['gcc', 'g++']), ['gcc', 'g++'])

    def test_partial_clang_pair_does_not_mix_compilers(self):
        self.assertEqual(self.select(['clang', 'gcc', 'g++']), ['gcc', 'g++'])

    def test_preference_disabled(self):
        self.assertEqual(self.select(['clang', 'clang++', 'gcc', 'g++'],
                                     setup='set(M0BOY_PREFER_CLANG OFF CACHE BOOL "")'), ['gcc', 'g++'])

    def test_c_only_needs_no_clangxx(self):
        self.assertEqual(self.select(['clang', 'gcc'], languages='C'), ['clang', ''])

    def test_explicit_or_cached_compiler_preserved(self):
        self.assertEqual(self.select(['clang', 'clang++'],
                                     setup='set(CMAKE_C_COMPILER /requested/gcc CACHE FILEPATH "")'), ['gcc', ''])

    def test_environment_preserved(self):
        for variable in ('CC', 'CXX', 'CMAKE_TOOLCHAIN_FILE'):
            with self.subTest(variable=variable):
                self.assertEqual(self.select(['clang', 'clang++'], env_overrides={variable: '/requested/tool'}), ['', ''])

    def test_toolchain_preserved(self):
        self.assertEqual(self.select(['clang', 'clang++'], setup='set(CMAKE_TOOLCHAIN_FILE custom.cmake)'), ['', ''])

    def test_platform_defaults_without_either_pair(self):
        self.assertEqual(self.select([]), ['', ''])


if __name__ == '__main__':
    unittest.main(argv=[__file__] + unittest_args)
