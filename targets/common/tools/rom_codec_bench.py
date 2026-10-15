#!/usr/bin/env python3
"""Independent ROM sectors: production LZSS (C/Thumb-1) and raw DEFLATE.

Requires the m0bench Python dependencies. Estimates, not device measurements.
Example: python rom_codec_bench.py game.gg --output out/codec-bench
The output contains ROM-derived bytes; keep it outside tracked/release files.
"""
import argparse
import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
DRIVER = r'''
#include <stdint.h>
#include "m0_rom_codec.h"
#include "corpus.h"
volatile uint32_t sector, sink;
uint8_t output[514];
#if MODE == 2
static m0_inflate_workspace workspace;
#endif
__attribute__((noinline)) void sector_done(void) { __asm__ volatile(""); }
__attribute__((noinline)) void bench_done(void) { for (;;) __asm__ volatile(""); }
void bench_main(void) {
    for (sector = 0; sector < SECTORS; ++sector) {
        uint32_t n = offsets[sector + 1] - offsets[sector];
        if (n == 512) continue;
        output[0] = 0xa5; output[513] = 0xa5;
#if MODE == 2
        if (!m0_rom_inflate(blob + offsets[sector], n, output + 1, 512, &workspace)) __builtin_trap();
#elif MODE == 1
        m0_rom_unpack512(blob + offsets[sector], output + 1);
#else
        m0_rom_unpack512_c(blob + offsets[sector], output + 1);
#endif
        sector_done();
        sink += output[1];
    }
    bench_done();
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', type=Path)
    parser.add_argument('--skip', type=int, default=0)
    parser.add_argument('--sectors', type=int, default=512)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cmake', '-G', 'Ninja', '-S', str(ROOT), '-B', str(args.build), '-DBUILD_TESTS=ON'], check=True)
    subprocess.run(['ninja', '-C', str(args.build), 'rom_codec_bench_data'], check=True)
    subprocess.run([str(args.build / 'bin/rom_codec_bench_data'), str(args.rom.resolve()),
                    str(args.skip), str(args.sectors), str(out)], check=True)
    bench = ROOT / 'targets/meta/tools/m0bench'
    spec = importlib.util.spec_from_file_location('estimate', bench / 'm0_estimate.py')
    estimate = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(estimate)
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB
    from elftools.elf.elffile import ELFFile
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_MCLASS, UC_MODE_THUMB, UC_HOOK_BLOCK, UC_HOOK_CODE
    from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR
    raw = (out / 'raw.bin').read_bytes()
    common = ROOT / 'targets/common/runtime'
    for mode, name in enumerate(('lzss-c', 'lzss-asm', 'deflate')):
        folder = out / name
        folder.mkdir(exist_ok=True)
        (folder / 'driver.c').write_text(DRIVER)
        (folder / 'corpus.h').write_bytes((out / ('deflate.h' if mode == 2 else 'lzss.h')).read_bytes())
        elf_path = folder / 'bench.elf'
        subprocess.run([estimate.find_gcc(), '-mcpu=cortex-m0plus', '-mthumb', '-O2', '-g',
                        '-ffunction-sections', '-fdata-sections', '-fstack-usage', '-nostartfiles',
                        '--specs=nano.specs', '-Wl,--gc-sections,-e,bench_main', '-DMODE=%d' % mode,
                        '-I', str(common), '-I', str(folder), '-T', str(bench / 'bench.ld'),
                        str(folder / 'driver.c'), str(common / 'm0_rom_lzss.S'), '-o', str(elf_path)], check=True,
                       cwd=folder)
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0, 0x80000)
        uc.mem_map(0x20000000, 0x8000)
        with elf_path.open('rb') as f:
            elf = ELFFile(f)
            for segment in elf.iter_segments():
                if segment['p_type'] == 'PT_LOAD' and segment['p_filesz']:
                    uc.mem_write(segment['p_paddr'], segment.data())
                    uc.mem_write(segment['p_vaddr'], segment.data())
            symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
            text = elf.get_section_by_name('.text')['sh_size']
            ram = sum(elf.get_section_by_name(n)['sh_size'] for n in ('.data', '.bss'))
        uc.mem_write(0x20007000, bytes([0xa5]) * 4096)
        uc.reg_write(UC_ARM_REG_SP, 0x20008000)
        uc.reg_write(UC_ARM_REG_LR, symbols['bench_done'] | 1)
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        costs, total, checked = {}, [0.0], [0]

        def block(u, address, size, _):
            key = address, size
            if key not in costs:
                costs[key] = sum(c + (0 if address >= 0x20000000 else w)
                                 for insn in md.disasm(bytes(u.mem_read(address, size)), address)
                                 for c, w in [estimate.classify(insn)])
            total[0] += costs[key]

        def verify(u, address, size, _):
            s = int.from_bytes(u.mem_read(symbols['sector'], 4), 'little')
            actual = bytes(u.mem_read(symbols['output'], 514))
            assert actual[0] == actual[-1] == 0xa5, (name, s, 'bounds')
            assert actual[1:-1] == raw[s * 512:(s + 1) * 512], (name, s, 'output')
            checked[0] += 1

        uc.hook_add(UC_HOOK_BLOCK, block)
        done = symbols['sector_done'] & ~1
        uc.hook_add(UC_HOOK_CODE, verify, begin=done, end=done)
        uc.emu_start(symbols['bench_main'] | 1, symbols['bench_done'] & ~1, count=500_000_000)
        from unicorn.arm_const import UC_ARM_REG_PC
        assert uc.reg_read(UC_ARM_REG_PC) == symbols['bench_done'] & ~1, 'instruction limit reached'
        assert checked[0], 'no compressed sectors exercised'
        stack = bytes(uc.mem_read(0x20007000, 4096))
        first = next((i for i, b in enumerate(stack) if b != 0xa5), 4096)
        print('%s: %d sectors verified; %.0f estimated cycles/sector; text+ROM %d B; static RAM %d B; stack high-water %d B' %
              (name, checked[0], total[0] / checked[0], text, ram, 4096 - first))


if __name__ == '__main__':
    main()
