#!/usr/bin/env python3
"""Execute generated Thumb plans against independent C instruction sequences.

Builds only through CMake/Ninja; Unicorn compares the entire CPU state,
including untouched flags/timing, for seeded entry values. Exit 77 is an
explicit skip when the optional ARM test dependencies are unavailable.
"""
import argparse
from pathlib import Path
import random
import shutil
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ir-test', required=True)
    ap.add_argument('--runtime', required=True)
    ap.add_argument('--compiler', choices=('gcc', 'clang'), default='gcc')
    args = ap.parse_args()
    gcc = shutil.which('arm-none-eabi-gcc')
    if not gcc:
        candidate = Path.home() / '.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-gcc'
        if candidate.exists():
            gcc = str(candidate)
    try:
        from elftools.elf.elffile import ELFFile
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
        from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_SP, UC_ARM_REG_PC
    except ImportError:
        print('SKIP: native register IR needs Unicorn and pyelftools')
        return 77
    if not gcc:
        print('SKIP: native register IR needs arm-none-eabi-gcc')
        return 77

    clang = shutil.which('clang')
    if args.compiler == 'clang' and not clang:
        print('SKIP: Clang compiler not found')
        return 77
    with tempfile.TemporaryDirectory(prefix='m0_register_ir_') as td:
        root = Path(td)
        subprocess.run([args.ir_test, '--emit-native', str(root / 'cases.c')], check=True)
        (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(register_ir_native C)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(CMAKE_C_LINK_EXECUTABLE "${GNU_CC} <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
endif()
add_executable(cases cases.c)
target_include_directories(cases PRIVATE "${RUNTIME}")
target_compile_options(cases PRIVATE -mcpu=cortex-m0plus -mthumb -ffreestanding)
target_link_options(cases PRIVATE -mcpu=cortex-m0plus -mthumb -nostdlib
    -Wl,-Ttext=0x10000 -Wl,--section-start=.bss=0x20000000 -Wl,-e,native_case_0)
''')
        for opt in ('-O0', '-O2', '-Os'):
            build = root / opt[1:]
            flags = opt
            compiler_options = ['-DCMAKE_C_COMPILER=' + gcc]
            if args.compiler == 'clang':
                sysroot = subprocess.check_output([gcc, '-print-sysroot'], text=True).strip()
                assembler = subprocess.check_output([gcc, '-print-prog-name=as'], text=True).strip()
                assembler_dir = Path(assembler).resolve().parent if Path(assembler).is_file() else Path(gcc).parent
                flags += ' --sysroot=' + sysroot + ' -B' + str(assembler_dir) + ' -fno-integrated-as -fshort-enums'
                compiler_options = ['-DCMAKE_C_COMPILER=' + clang, '-DCMAKE_C_COMPILER_TARGET=arm-none-eabi',
                                    '-DGNU_CC=' + gcc]
            subprocess.run(['cmake', '-G', 'Ninja', '-S', str(root), '-B', str(build),
                            '-DRUNTIME=' + args.runtime, '-DCMAKE_C_FLAGS=' + flags] + compiler_options,
                           check=True, stdout=subprocess.DEVNULL)
            subprocess.run(['ninja', '-C', str(build)], check=True)
            uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
            uc.mem_map(0x10000, 0x100000)
            uc.mem_map(0x20000000, 0x10000)
            with (build / 'cases').open('rb') as f:
                elf = ELFFile(f)
                for sec in elf.iter_sections():
                    if sec['sh_flags'] & 2 and sec['sh_type'] != 'SHT_NOBITS':
                        uc.mem_write(sec['sh_addr'], sec.data())
                symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            stop = 0x10F000
            rng = random.Random(1989)
            for case in range(256):
                for _ in range(32):
                    state = bytes(rng.randrange(256) for _ in range(28))
                    results = []
                    for name in ('native_case_', 'reference_case_'):
                        uc.mem_write(symbols['gbr'], state)
                        uc.reg_write(UC_ARM_REG_SP, 0x2000F000)
                        uc.reg_write(UC_ARM_REG_LR, stop | 1)
                        uc.emu_start(symbols[name + str(case)] | 1, stop, count=10000)
                        assert uc.reg_read(UC_ARM_REG_PC) == stop, "native function did not return"
                        results.append(bytes(uc.mem_read(symbols['gbr'], 28)))
                    if results[0] != results[1]:
                        raise AssertionError(f'{opt}, case {case}: {results[0].hex()} != {results[1].hex()}')
            print(f'{opt}: 8,192 native/reference CPU states identical')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
