#!/usr/bin/env python3
"""Compiler dispatch must preserve GNU assembly/linking and fail explicitly."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('arm_clang', ROOT / 'targets/common/toolchain/arm_clang.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
compat_spec = importlib.util.spec_from_file_location('cmsis_compat', ROOT / 'targets/common/toolchain/cmsis_compat.py')
compat = importlib.util.module_from_spec(compat_spec)
compat_spec.loader.exec_module(compat)


class CompilerSelection(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='m0 compiler ')
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.gcc = root / 'arm-none-eabi-gcc'
        self.gcc.write_text('#!/bin/sh\nprintf "%s\\n" "' + str(root) + '"\n')
        self.gcc.chmod(0o755)
        self.clang = root / 'clang'
        self.clang.write_text('#!/bin/sh\nexit 0\n')
        self.clang.chmod(0o755)

    def test_assembly_and_link_keep_original_arguments(self):
        for args in (['-c', 'runtime.S', '-o', 'runtime.o'], ['--specs=nano.specs', 'main.o', '-o', 'firmware.elf'],
                     ['-E', '-P', '-x', 'c', 'pokitto.ld']):
            self.assertEqual(module.clang_command(str(self.gcc), 'c', args), [str(self.gcc)] + args)
        self.assertEqual(module.clang_command(str(self.gcc), 'c++', ['main.o'])[0],
                         str(self.gcc).replace('gcc', 'g++'))

    def test_clang_keeps_paths_and_defines_and_drops_gcc_only_options(self):
        with patch.dict(os.environ, {'CLANG': str(self.clang)}):
            cmd = module.clang_command(str(self.gcc), 'c',
                                       ['-O2', '--param', 'max-inline-insns-single=500', '--specs=nano.specs',
                                        '-DVALUE=42', '-c', 'source with spaces.c', '-o', 'output with spaces.o'])
        self.assertEqual(cmd[0], str(self.clang))
        self.assertIn('--target=arm-none-eabi', cmd)
        self.assertIn('-fshort-enums', cmd)
        self.assertIn('--sysroot=' + self.tmp.name, cmd)
        self.assertIn('-DVALUE=42', cmd)
        self.assertIn('source with spaces.c', cmd)
        self.assertNotIn('--param', cmd)
        self.assertNotIn('--specs=nano.specs', cmd)

    def test_missing_clang_does_not_silently_fall_back(self):
        with patch.dict(os.environ, {'CLANG': '/missing/m0-clang'}):
            with self.assertRaisesRegex(ValueError, 'Clang not found'):
                module.clang_command(str(self.gcc), 'c', ['-c', 'main.c'])

    def test_distro_toolchain_selects_target_assembler(self):
        assembler = Path(self.tmp.name) / 'target/bin/as'
        assembler.parent.mkdir(parents=True)
        assembler.write_text('#!/bin/sh\nexit 0\n')
        assembler.chmod(0o755)
        self.gcc.write_text('#!/bin/sh\ncase "$1" in\n-print-prog-name=as) printf "%s\\n" "' + str(assembler) + '";;\n*) printf "%s\\n" "' + self.tmp.name + '";;\nesac\n')
        with patch.dict(os.environ, {'CLANG': str(self.clang)}):
            command = module.clang_command(str(self.gcc), 'c', ['-c', 'main.c'])
        self.assertIn('-B' + str(assembler.parent), command)

    def test_cmsis_changes_only_unnamed_reserved_qualifiers(self):
        original = "    __I uint32_t :2; /* reserved */\n    __I uint32_t STATUS:2;\n    __IO uint8_t :1;\n"
        converted = compat.sanitize_reserved_fields(original)
        self.assertEqual(converted, "     uint32_t :2; /* reserved */\n    __I uint32_t STATUS:2;\n     uint8_t :1;\n")
        self.assertEqual(compat.sanitize_reserved_fields(converted), converted)

    def test_cmsis_copy_preserves_sdk_and_unchanged_timestamps(self):
        root = Path(self.tmp.name)
        sdk, dest = root / 'sdk', root / 'compat'
        (sdk / 'samd21/include/component').mkdir(parents=True)
        (sdk / 'sam.h').write_text('#include "samd.h"\n')
        (sdk / 'samd.h').write_text('#include "samd21/include/device.h"\n')
        field = sdk / 'samd21/include/component/register.h'
        field.write_text('__I uint32_t :2;\n__I uint32_t STATUS:2;\n')
        compat.prepare_cmsis(sdk, dest)
        copied = dest / 'samd21/include/component/register.h'
        first_mtime = copied.stat().st_mtime_ns
        compat.prepare_cmsis(sdk, dest)
        self.assertTrue((dest / 'samd.h').is_file())
        self.assertEqual(copied.stat().st_mtime_ns, first_mtime)
        self.assertTrue(field.read_text().startswith('__I uint32_t :2;'))
        self.assertTrue(copied.read_text().startswith(' uint32_t :2;'))

    def test_llvm_lto_cannot_reach_gnu_linker(self):
        with self.assertRaisesRegex(ValueError, 'LLVM LTO is unsupported'):
            module.clang_command(str(self.gcc), 'c', ['-flto', '-c', 'main.c'])


if __name__ == '__main__':
    unittest.main()
