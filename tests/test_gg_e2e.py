"""Synthetic Game Gear/Master System cartridge (tests/gg_fixture.py): banked compiled code,
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


def exercise(args, root, system):
    root.mkdir()
    rom = root / ('synthetic.' + system)
    rom.write_bytes(fixture())
    coverage = root / 'synthetic.cov'
    common = ('--frames', 60, '--input', INPUT if system == 'gg' else '20:128:5,30:16:10,40:8:20')
    baseline = run(args.host, rom, *common, '--coverage', coverage,
                   '--shot', root / 'reference.ppm', '--wav', root / 'reference.wav')
    text = coverage.read_text()
    assert text.startswith('# m0boy.' + system + '.coverage.v1\n') and '# read ' in text
    assert re.search(r'^02[0-9a-f]{4} \d+$', text, re.M) and re.search(r'^03[0-9a-f]{4} \d+$', text, re.M)  # pages 2 and 3
    # The coverage-guided explorer: segments from snapshots, a profile with rates
    explored = root / 'explored.cov'
    run(args.host, rom, '--explore', 1500, '--random', 3, '--coverage', explored)
    text = explored.read_text()
    played = re.search(r'^# frames (\d+)$', text, re.M)  # the last segment ends past 1500 (2400 frames at most)
    assert played and 1500 <= int(played.group(1)) < 1500 + 2400 and '# read ' in text
    assert re.search(r'^[0-9a-f]{6} \d+$', text, re.M)
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
        print(system + '/' + target + ': precise native = interpreter (hashes, pixels, audio, fallback); default build runs')
    invalid = root / 'wrong.cov'
    invalid.write_text('# m0boy.sg1000.coverage.v1\n0000\n')
    failed = subprocess.run([args.recompiler, str(rom), '--coverage', str(invalid),
                             '-o', str(root / 'invalid')], capture_output=True)
    assert failed.returncode != 0 and not (root / 'invalid').exists()


STUB = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* The SD card of a streamed project, here the cartridge file */
int gg_platform_rom_read(uint32_t sector, uint8_t *dst) {
    static FILE *file;
    if (!file) { file = fopen(ROM_FILE, "rb"); FILE *mark = fopen(ROM_FILE ".read", "wb"); if (mark) fclose(mark); }
    if (!file || fseek(file, (long)sector * 512, SEEK_SET) || fread(dst, 1, 512, file) != 512) { memset(dst, 0xff, 512); return 0; }
    return 1;
}
"""


def exercise_pool(args, root):
    """A cartridge larger than the flash: the sectors the profile reads most stay in
    flash (the pool), the others come from the card; same run as the interpreter."""
    root.mkdir()
    rom = root / 'big.gg'
    rom.write_bytes(fixture().ljust(20 * 0x4000, b'\xa5'))  # 320 KiB, more than the flash holds
    coverage = root / 'big.cov'
    common = ('--frames', 60, '--input', INPUT)
    baseline = run(args.host, rom, *common, '--coverage', coverage)
    text = coverage.read_text()
    assert '# frames 60\n' in text and re.search(r'^02[0-9a-f]{4} \d+$', text, re.M)  # executions are counted
    reads = [l for l in text.splitlines() if l.startswith('# read ')]
    assert reads
    # Page 3's sectors are not in the profile (they stay on the card); the others are hot.
    kept = [l for l in text.splitlines() if not (l.startswith('# read ') and 0x60 <= int(l.split()[2], 16) < 0x80)]
    coverage.write_text('\n'.join(kept) + '\n')
    for target in ('pokitto', 'meta'):
        output = root / target
        log = run(args.recompiler, rom, '--target', target, '--coverage', coverage, '--meta-precise',
                  '--output-prefix', 'big', '-o', output)
        assert re.search(r'ROM streaming: +\d+ pages of 16 KiB read from the SD card, \d+ hot sectors', log), log
        config = (output / 'src/sg_config.h').read_text()
        pool = int(re.search(r'#define GG_POOL_SECTORS (\d+)', config)[1])
        assert 0 < pool <= 254
        data = (output / 'src/sg_rom.c').read_text()
        assert 'gg_pool_index[640]' in data and 'gg_pool[%d]' % (pool * 512) in data
        src = output / 'src'
        (root / (target + '_stub.c')).write_text('#define ROM_FILE "%s"\n' % rom + STUB)
        exe = root / (target + '_host')
        run(shutil.which('cc') or 'cc', '-O1', '-DSG_GENERATED=1', '-I', src, '-o', exe, output / 'host/gg_host.c',
            *(src / n for n in ('z80.c', 'gg.c', 'gg_psg.c', 'gg_vdp.c', 'sg_gen.c', 'sg_rom.c')),
            root / (target + '_stub.c'), '-lm')
        native = run(exe, *common)
        interpreted = run(exe, *common, '--interp')
        assert fingerprint(native) == fingerprint(interpreted) == fingerprint(baseline)
        assert int(re.search(r'compiled=(\d+)', native)[1]) > 0
        assert (root / 'big.gg.read').exists()  # some sectors do come from the card
        print('gg/%s: %d-sector pool and the card give the interpreter\'s run' % (target, pool))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--recompiler', required=True)
    parser.add_argument('--host', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='m0_gg_') as temporary:
        root = Path(temporary)
        for system in ('gg', 'sms'):
            exercise(args, root / system, system)
        exercise_pool(args, root / 'pool')


if __name__ == '__main__':
    main()
