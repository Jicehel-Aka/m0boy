#!/usr/bin/env python3
"""Clang C/C++ compilation with the existing GNU ARM ABI and libraries.

Assembly/preprocessing/linking use GNU tools. This lets the firmware retain
its .S sources, newlib-nano, linker scripts, and binary conversion unchanged.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def find_gcc(requested=None):
    requested = requested or 'arm-none-eabi-gcc'
    found = shutil.which(requested)
    if not found:
        candidate = Path(requested).expanduser()
        if candidate.is_file():
            found = str(candidate)
    if not found and requested == 'arm-none-eabi-gcc':
        candidate = Path.home() / '.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-gcc'
        if candidate.is_file():
            found = str(candidate)
    if not found:
        raise ValueError('GNU ARM toolchain not found: ' + requested)
    return str(Path(found).absolute())


def gnu_cpp(gcc):
    return gcc.replace('gcc.exe', 'g++.exe') if gcc.endswith('gcc.exe') else gcc[:-3] + 'g++'


def clang_command(gcc, language, arguments):
    gcc = find_gcc(gcc)
    cpp = language == 'c++'
    driver = gnu_cpp(gcc) if cpp else gcc
    # The preprocessed linker script and assembly need GNU preprocessing.
    assembly = any(a.endswith(('.S', '.s')) for a in arguments) or any(
        a.startswith('assembler') for a in arguments)
    if assembly or '-E' in arguments or '-c' not in arguments:
        return [driver] + arguments
    if any(a.startswith('-flto') for a in arguments):
        raise ValueError('Clang mode uses the GNU linker: LLVM LTO is unsupported; remove -flto')
    compiler = os.environ.get('CLANGXX' if cpp else 'CLANG', 'clang++' if cpp else 'clang')
    compiler = shutil.which(compiler)
    if not compiler:
        raise ValueError('Clang not found: install clang/clang++, or set CLANG/CLANGXX')
    sysroot = subprocess.check_output([gcc, '-print-sysroot'], text=True).strip()
    if not sysroot:
        raise ValueError('GNU ARM compiler did not report a sysroot')
    command = [compiler, '--target=arm-none-eabi', '--sysroot=' + sysroot,
               '-B' + str(Path(gcc).parent), '-fno-integrated-as', '-fshort-enums',
               '-Wno-unknown-pragmas', '-Wno-unused-command-line-argument']
    if cpp:
        if not any(arg.startswith('-std=') for arg in arguments):
            command.append('-std=gnu++11')
        # Use the target C++ headers, never the host's standard library.
        probe = subprocess.run([driver, '-mcpu=cortex-m0plus', '-mthumb', '-E', '-x', 'c++', '-', '-v'],
                               input='', text=True, capture_output=True, check=True)
        collecting = False
        for line in probe.stderr.splitlines():
            if '#include <...> search starts here:' in line:
                collecting = True
            elif 'End of search list.' in line:
                collecting = False
            elif collecting and Path(line.strip()).is_dir():
                command += ['-isystem', line.strip()]
    filtered = []
    i = 0
    while i < len(arguments):
        arg = arguments[i]
        if arg == '--param':
            i += 2  # GCC-specific inlining threshold; LLVM chooses its own.
            continue
        if arg.startswith(('--param=', '--specs=', '-specs=')):
            i += 1
            continue
        filtered.append(arg)
        i += 1
    return command + filtered


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--gcc', default=None)
    ap.add_argument('--language', choices=('c', 'c++'), default='c')
    ap.add_argument('arguments', nargs=argparse.REMAINDER)
    args = ap.parse_args()
    arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
    try:
        return subprocess.call(clang_command(args.gcc, args.language, arguments))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('ARM Clang: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
