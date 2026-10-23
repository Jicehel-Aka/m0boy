"""Synthetic MSX cartridge: simulated BIOS, slots, PPI, AY, VDP, compiled and
fallback CPU paths, and the generated projects of every destination.

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
from msx_fixture import fixture, text_fixture, mapper_fixture, mapper_expected, MAPPERS  # noqa: E402


def run(*args):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def fingerprint(output):
    return re.search(r'frames=(\d+) cycles=(\d+) pc=([0-9a-f]+).*run_hash=([0-9a-f]+)', output).groups()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--recompiler', required=True)
    parser.add_argument('--host', required=True, help='msx_host: the interpreter-only harness')
    parser.add_argument('--runtime-test', required=True, help='test_msx_runtime: machine state after the fixture')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='m0_msx_') as temporary:
        root = Path(temporary)
        rom = root / 'synthetic.mx1'
        rom.write_bytes(fixture())
        text = root / 'text.mx1'
        text.write_bytes(text_fixture())
        assert 'ok' in run(args.runtime_test, rom, text)
        info = run(args.recompiler, rom, '--info')
        assert 'system=msx' in info and 'rom_bytes=16384' in info and 'mapper=unbanked 4000-BFFF' in info, info
        # a ROM without the AB header is refused, not run
        bad = root / 'plain.mx1'
        bad.write_bytes(bytes(16384))
        failed = subprocess.run([args.recompiler, str(bad), '-o', str(root / 'bad')], capture_output=True)
        assert failed.returncode != 0 and not (root / 'bad').exists()
        common = ('--frames', 24, '--buttons', 0x11)
        coverage = root / 'synthetic.cov'
        baseline = run(args.host, rom, *common, '--coverage', coverage,
                       '--shot', root / 'reference.ppm', '--wav', root / 'reference.wav')
        assert coverage.read_text().startswith('# m0boy.msx.coverage.v2')
        wav = (root / 'reference.wav').read_bytes()[44:]
        assert len(set(wav)) > 1, 'the AY produced no sound'
        assert len(set((root / 'reference.ppm').read_bytes()[15:])) > 3, 'the screen is blank'
        for target in ('desktop', 'meta', 'pokitto'):
            output = root / target
            # Precise build (sync before every instruction): the recompiled
            # code must reproduce the interpreter exactly.
            run(args.recompiler, rom, '--target', target, '--coverage', coverage,
                '--meta-precise', '--output-prefix', 'synthetic', '-o', output)
            host = output / 'host/build'
            run('cmake', '-G', 'Ninja', '-S', output / 'host', '-B', host)
            run('ninja', '-C', host)
            exe = host / 'synthetic_host'
            native = run(exe, *common, '--shot', root / 'native.ppm', '--wav', root / 'native.wav')
            interpreted = run(exe, *common, '--interp', '--wav', root / 'interp.wav')
            assert fingerprint(native) == fingerprint(interpreted)
            if target == 'desktop':  # same RAM as the generic harness
                assert fingerprint(native) == fingerprint(baseline)
            assert (root / 'native.wav').read_bytes() == (root / 'interp.wav').read_bytes() == (root / 'reference.wav').read_bytes()
            assert (root / 'native.ppm').read_bytes() == (root / 'reference.ppm').read_bytes()
            assert int(re.search(r'compiled=(\d+)', native)[1]) > 0
            assert 'out_mismatch=0' in native
            # An almost-empty native budget: the interpreter must keep all state.
            run(args.recompiler, rom, '--target', target, '--coverage', coverage, '--meta-precise',
                '--meta-code-budget', 160, '--output-prefix', 'synthetic', '-o', output)
            run('ninja', '-C', host)
            mixed = run(exe, *common)
            assert fingerprint(mixed) == fingerprint(interpreted)
            assert int(re.search(r'interpreted=(\d+)', mixed)[1]) > 0
            # Default build: syncs at loop heads, flags only where read.
            fast_output = root / (target + '-fast')
            run(args.recompiler, rom, '--target', target, '--coverage', coverage,
                '--output-prefix', 'synthetic', '-o', fast_output)
            fast_host = fast_output / 'host/build'
            run('cmake', '-G', 'Ninja', '-S', fast_output / 'host', '-B', fast_host)
            run('ninja', '-C', fast_host)
            fast = run(fast_host / 'synthetic_host', *common)
            assert re.search(r'frames=24 ', fast) and 'out_mismatch=0' in fast
            assert int(re.search(r'compiled=(\d+)', fast)[1]) > 0
            if target == 'pokitto' and shutil.which('arm-none-eabi-gcc'):
                build = fast_output / 'build'
                run('cmake', '-G', 'Ninja', '-S', fast_output, '-B', build,
                    '-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake')
                run('ninja', '-C', build)
                binary = (build / 'firmware.bin').read_bytes()
                assert len(binary) < 0x39000
                assert sum(struct.unpack_from('<8I', binary)) & 0xffffffff == 0
            print(target + ': precise native = interpreter (hashes, pixels, audio, fallback); default build runs')
        for name in MAPPERS:
            banked(args, root, name)


def banked(args, root, name):
    """a 256 KiB cartridge on one mapper: bank registers, code in banked windows, the packed
    ROM of the consoles (it does not fit their flash raw) and the interpreter fallback"""
    rom = root / (name + '.mx1')
    rom.write_bytes(mapper_fixture(name))
    expected = mapper_expected(name)
    peeks = [a for address in sorted(expected) for a in ('--peek', address)]
    common = ('--frames', 12, '--mapper', name, *peeks)
    coverage = root / (name + '.cov')
    baseline = run(args.host, rom, *common, '--coverage', coverage)
    for address, value in expected.items():
        assert f'peek {address:04x}={value:02x}' in baseline, (name, hex(address), value, baseline)
    assert '# bank-shift 13' in coverage.read_text()
    assert f'mapper={name}' in run(args.recompiler, rom, '--info', '--msx-mapper', name) or name in run(args.recompiler, rom, '--info')
    for target in ('desktop', 'meta', 'pokitto'):
        output = root / (name + '-' + target)
        run(args.recompiler, rom, '--target', target, '--coverage', coverage, '--meta-precise',
            '--msx-mapper', name, '--output-prefix', 'synthetic', '-o', output)
        host = output / 'host/build'
        run('cmake', '-G', 'Ninja', '-S', output / 'host', '-B', host)
        run('ninja', '-C', host)
        exe = host / 'synthetic_host'
        native = run(exe, '--frames', 12, *peeks)
        interpreted = run(exe, '--frames', 12, '--interp', *peeks)
        assert fingerprint(native) == fingerprint(interpreted)
        for address, value in expected.items():
            assert f'peek {address:04x}={value:02x}' in native, (name, target, hex(address), native)
        assert int(re.search(r'compiled=(\d+)', native)[1]) > 0 and 'out_mismatch=0' in native
        config = (output / 'src/sg_config.h').read_text()
        assert ('#define SG_MSX_PACKED ' + ('0' if target == 'desktop' else '1')) in config
        print(name + ' ' + target + ': banked, precise native = interpreter' + ('' if target == 'desktop' else ', ROM packed in flash'))


if __name__ == '__main__':
    main()
