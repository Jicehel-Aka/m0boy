"""Synthetic SG cartridge: compiled/fallback CPU paths and real M0 firmware.

No ROM or ROM-derived fixture is kept in the repository.
"""
import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


def run(*args):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def fixture():
    rom = bytearray(16384)
    # Reset jumps over IM1 and NMI vectors.
    rom[0:3] = bytes([0xc3, 0x00, 0x01])
    # IRQ increments a mirrored RAM counter, acknowledges VDP status, EI; RETI.
    irq = bytes([0xf5, 0x3a, 0x00, 0xc0, 0x3c, 0x32, 0x00, 0xc0,
                 0xdb, 0xbf, 0xf1, 0xfb, 0xed, 0x4d])
    rom[0x38:0x38 + len(irq)] = irq
    nmi = bytes([0xf5, 0x3a, 0x01, 0xc0, 0x3c, 0x32, 0x01, 0xc0, 0xf1, 0xed, 0x45])
    rom[0x66:0x66 + len(nmi)] = nmi
    code = bytearray([0xf3, 0x31, 0xff, 0xc3, 0xed, 0x56])
    # TMS register 1: display and IRQ enabled. Graphics I tables/sprite end.
    for register, value in ((1, 0x60), (2, 6), (3, 0x80), (4, 0), (5, 0x36), (7, 1)):
        code += bytes([0x3e, value, 0xd3, 0xbf, 0x3e, 0x80 | register, 0xd3, 0xbf])
    for address, value in ((0, 0xaa), (0x2000, 0xf2), (0x1b00, 208)):
        code += bytes([0x3e, address & 255, 0xd3, 0xbf, 0x3e, 0x40 | (address >> 8),
                       0xd3, 0xbf, 0x3e, value, 0xd3, 0xbe])
    code += bytes([0xdd, 0x21, 0x10, 0xc0, 0xdd, 0x36, 0xfe, 0x42,
                   0xdd, 0xcb, 0xfe, 0x00, 0xfd, 0x21, 0x12, 0xc0,
                   0xfd, 0x70, 0x01, 0xfb])
    # Tone 0: 440 Hz, loud. Tone divider 254 (low nibble + high six bits).
    for value in (0x8e, 0x0f, 0x90):
        code += bytes([0x3e, value, 0xd3, 0x7f])
    loop = 0x100 + len(code)
    # Busy loop spanning raster boundaries exercises native fallthrough,
    # interrupt/EI timing and stores, rather than spending every frame in HALT.
    code += bytes([0x3a, 0x02, 0xc0, 0x3c, 0x32, 0x02, 0xc0]) + bytes(40)
    code += bytes([0xc3, loop & 255, loop >> 8])
    rom[0x100:0x100 + len(code)] = code
    return rom


def fingerprint(output):
    return re.search(r'frames=(\d+) cycles=(\d+) pc=([0-9a-f]+).*run_hash=([0-9a-f]+)', output).groups()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--recompiler', required=True)
    parser.add_argument('--host', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='m0_sg1000_') as temporary:
        root = Path(temporary)
        rom = root / 'synthetic.sg'
        rom.write_bytes(fixture())
        coverage = root / 'synthetic.cov'
        baseline = run(args.host, rom, '--frames', 24, '--pause-frame', 8,
                       '--coverage', coverage, '--shot', root / 'reference.ppm', '--wav', root / 'reference.wav')
        for target in ('desktop', 'meta', 'pokitto'):
            # Precise build (sync before every instruction): the recompiled
            # code must reproduce the interpreter exactly.
            output = root / target
            run(args.recompiler, rom, '--target', target, '--coverage', coverage,
                '--meta-precise', '--output-prefix', 'synthetic', '-o', output)
            host = output / 'host/build'
            run('cmake', '-G', 'Ninja', '-S', output / 'host', '-B', host)
            run('ninja', '-C', host)
            exe = host / 'synthetic_host'
            native = run(exe, '--frames', 24, '--pause-frame', 8, '--shot', root / 'native.ppm', '--wav', root / 'native.wav')
            interpreted = run(exe, '--frames', 24, '--pause-frame', 8, '--interp', '--wav', root / 'interp.wav')
            assert fingerprint(native) == fingerprint(interpreted) == fingerprint(baseline)
            assert (root / 'native.wav').read_bytes() == (root / 'interp.wav').read_bytes() == (root / 'reference.wav').read_bytes()
            assert len(set((root / 'native.wav').read_bytes()[44:])) > 1
            assert (root / 'native.ppm').read_bytes() == (root / 'reference.ppm').read_bytes()
            assert int(re.search(r'compiled=(\d+)', native)[1]) > 0
            # Force an almost-empty native budget: fallback must retain all state.
            run(args.recompiler, rom, '--target', target, '--coverage', coverage, '--meta-precise',
                '--meta-code-budget', 160, '--output-prefix', 'synthetic', '-o', output)
            run('ninja', '-C', host)
            mixed = run(exe, '--frames', 24, '--pause-frame', 8)
            assert fingerprint(mixed) == fingerprint(baseline)
            assert int(re.search(r'interpreted=(\d+)', mixed)[1]) > 0
            # Default build: syncs at loop heads, flags only where read.
            fast_output = root / (target + '-fast')
            run(args.recompiler, rom, '--target', target, '--coverage', coverage,
                '--output-prefix', 'synthetic', '-o', fast_output)
            fast_host = fast_output / 'host/build'
            run('cmake', '-G', 'Ninja', '-S', fast_output / 'host', '-B', fast_host)
            run('ninja', '-C', fast_host)
            fast = run(fast_host / 'synthetic_host', '--frames', 24, '--pause-frame', 8, '--wav', root / 'fast.wav')
            assert re.search(r'frames=24 ', fast) and 'out_mismatch=0' in fast
            assert int(re.search(r'compiled=(\d+)', fast)[1]) > 0
            assert len(set((root / 'fast.wav').read_bytes()[44:])) > 1
            if target == 'pokitto' and shutil.which('arm-none-eabi-gcc'):
                build = fast_output / 'build'
                run('cmake', '-G', 'Ninja', '-S', fast_output, '-B', build,
                    '-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake')
                run('ninja', '-C', build)
                binary = (build / 'firmware.bin').read_bytes()
                assert len(binary) < 0x39000
                assert sum(struct.unpack_from('<8I', binary)) & 0xffffffff == 0
                # Linker assertions reserve 2 KiB of stack within the LPC RAM.
            print(target + ': precise native = interpreter (hashes, pixels, audio, fallback); default build runs')
        invalid = root / 'wrong.cov'
        invalid.write_text('# gbl coverage\n0000\n')
        failed = subprocess.run([args.recompiler, str(rom), '--coverage', str(invalid),
                                 '-o', str(root / 'invalid')], capture_output=True)
        assert failed.returncode != 0 and not (root / 'invalid').exists()


if __name__ == '__main__':
    main()
