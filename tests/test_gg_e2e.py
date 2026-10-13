"""Synthetic Game Gear cartridge (tests/gg_fixture.py): banked compiled code,
interpreter fallback and real M0 firmware.

No ROM or ROM-derived fixture is kept in the repository.
"""
import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gg_fixture import fixture  # noqa: E402


def run(*args):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def fingerprint(output):
    return re.search(r'frames=(\d+) cycles=(\d+) pc=([0-9a-f]+) page=([0-9,]+).*run_hash=([0-9a-f]+)', output).groups()


INPUT = '20:64:5,30:16:10,40:8:20'  # START, button 1, right


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--recompiler', required=True)
    parser.add_argument('--host', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='m0_gg_') as temporary:
        root = Path(temporary)
        rom = root / 'synthetic.gg'
        rom.write_bytes(fixture())
        coverage = root / 'synthetic.cov'
        common = ('--frames', 60, '--input', INPUT)
        baseline = run(args.host, rom, *common, '--coverage', coverage,
                       '--shot', root / 'reference.ppm', '--wav', root / 'reference.wav')
        text = coverage.read_text()
        assert text.startswith('# m0boy.gg.coverage.v1\n') and '# read ' in text
        assert re.search(r'^02[0-9a-f]{4}$', text, re.M) and re.search(r'^03[0-9a-f]{4}$', text, re.M)  # pages 2 and 3
        for target in ('desktop', 'meta', 'pokitto'):
            # Precise build: the recompiled code reproduces the interpreter,
            # mapper included.
            output = root / target
            run(args.recompiler, rom, '--target', target, '--coverage', coverage,
                '--meta-precise', '--output-prefix', 'synthetic', '-o', output)
            host = output / 'host/build'
            run('cmake', '-G', 'Ninja', '-S', output / 'host', '-B', host)
            run('ninja', '-C', host)
            exe = host / 'synthetic_host'
            native = run(exe, *common, '--shot', root / 'native.ppm', '--wav', root / 'native.wav')
            interpreted = run(exe, *common, '--interp', '--wav', root / 'interp.wav')
            assert fingerprint(native) == fingerprint(interpreted) == fingerprint(baseline)
            assert (root / 'native.wav').read_bytes() == (root / 'interp.wav').read_bytes() == (root / 'reference.wav').read_bytes()
            assert len(set((root / 'native.wav').read_bytes()[44:])) > 1
            assert (root / 'native.ppm').read_bytes() == (root / 'reference.ppm').read_bytes()
            assert int(re.search(r'compiled=(\d+)', native)[1]) > 0
            source = (output / 'src/sg_gen.c').read_text()
            assert 'machine.page[2] ==' in source  # guarded call into slot 2
            # An almost-empty native budget: fallback keeps every state.
            run(args.recompiler, rom, '--target', target, '--coverage', coverage, '--meta-precise',
                '--meta-code-budget', 160, '--output-prefix', 'synthetic', '-o', output)
            run('ninja', '-C', host)
            mixed = run(exe, *common)
            assert fingerprint(mixed) == fingerprint(baseline)
            assert int(re.search(r'interpreted=(\d+)', mixed)[1]) > 0
            # Default build: syncs at loop heads, flags only where read.
            fast_output = root / (target + '-fast')
            run(args.recompiler, rom, '--target', target, '--coverage', coverage,
                '--output-prefix', 'synthetic', '-o', fast_output)
            fast_host = fast_output / 'host/build'
            run('cmake', '-G', 'Ninja', '-S', fast_output / 'host', '-B', fast_host)
            run('ninja', '-C', fast_host)
            fast = run(fast_host / 'synthetic_host', *common, '--wav', root / 'fast.wav')
            assert re.search(r'frames=60 ', fast) and int(re.search(r'compiled=(\d+)', fast)[1]) > 0
            if target == 'pokitto' and shutil.which('arm-none-eabi-gcc'):
                build = fast_output / 'build'
                run('cmake', '-G', 'Ninja', '-S', fast_output, '-B', build,
                    '-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake')
                run('ninja', '-C', build)
                binary = (build / 'firmware.bin').read_bytes()
                assert len(binary) < 0x39000
                assert sum(struct.unpack_from('<8I', binary)) & 0xffffffff == 0
            print(target + ': precise native = interpreter (hashes, pixels, audio, fallback); default build runs')
        invalid = root / 'wrong.cov'
        invalid.write_text('# m0boy.sg1000.coverage.v1\n0000\n')
        failed = subprocess.run([args.recompiler, str(rom), '--coverage', str(invalid),
                                 '-o', str(root / 'invalid')], capture_output=True)
        assert failed.returncode != 0 and not (root / 'invalid').exists()


if __name__ == '__main__':
    main()
