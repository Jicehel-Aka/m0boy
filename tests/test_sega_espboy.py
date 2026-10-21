"""SG-1000, Game Gear and Master System on the ESPboy: generated projects (the
recompiled code reproduces the interpreter), the ROM split between the
firmware and rom.bin, and the cartridge RAM saved in flash (mock flash).
Optional real Xtensa build: ESPBOY_PIO=/path/to/pio.  Synthetic cartridges only.
"""
import argparse
import os
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gg_fixture import fixture as gg_fixture  # noqa: E402
from test_sg1000_e2e import fixture as sg_fixture, fingerprint as sg_fingerprint  # noqa: E402
from test_gg_e2e import run, fingerprint, INPUT  # noqa: E402

TESTS = Path(__file__).resolve().parent


def generate(args, rom, coverage, output, *extra):
    return run(args.recompiler, rom, '--target', 'espboy', '--coverage', coverage, '--meta-precise',
               '--output-prefix', 'synthetic', *extra, '-o', output)


def host_runs(output, common):
    host = output / 'host/build'
    run('cmake', '-G', 'Ninja', '-S', output / 'host', '-B', host)
    run('ninja', '-C', host)
    exe = host / 'synthetic_host'
    return run(exe, *common), run(exe, *common, '--interp')


def gm0_run(output, frames=240):
    """Optional (ESPBOY_GM0=/path/to/gm0): the firmware, Xtensa code and all,
    must run in gm0's ESPboy machine without stopping (timing there is an
    estimate)."""
    gm0 = os.environ.get('ESPBOY_GM0')
    if not gm0:
        return
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', EMU_NOPACE='1')
    cmd = [gm0, str(output / '.pio/build/espboy/firmware.bin'), '--frames', str(frames)]
    if (output / 'rom.bin').exists():
        cmd += ['--rom', str(output / 'rom.bin')]
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    assert result.returncode == 0 and 'ARRÊT' not in result.stderr, result.stderr[-800:]
    print('  runs in gm0')


def pio(output):
    tool = os.environ.get('ESPBOY_PIO')
    if tool:
        run(tool, 'run', '-d', output)
        size = (output / '.pio/build/espboy/firmware.bin').stat().st_size
        assert size <= 0x100000
        print('  Xtensa firmware:', size, 'bytes')
        gm0_run(output)


def small(args, root, system, data, common, host):
    root.mkdir()
    rom = root / ('synthetic.' + system)
    rom.write_bytes(data)
    coverage = root / 'synthetic.cov'
    baseline = run(host, rom, *common, '--coverage', coverage)
    output = root / 'out'
    generate(args, rom, coverage, output)
    config = (output / 'src/sg_config.h').read_text()
    assert re.search(r'#define SG_FLASH_GAME_ID \d+u', config) and '_PANEL_SWAP 1' in config, config
    assert not (output / 'rom.bin').exists()  # a small ROM is part of the firmware
    gbl = (output / 'src/gbl_config.h').read_text()
    assert '#define GBL_TARGET_ESPBOY 1' in gbl
    assert 'SG_ROM_ATTR' in (output / 'src/sg_rom.c').read_text()
    for name in ('espboy_hw.cpp', 'espboy_flash.cpp', 'gbl_espboy.h', 'meta_menu.h', 'platformio.ini', 'espboy.ld'):
        assert (output / name).exists() or (output / 'src' / name).exists(), name
    assert not list((output / 'src').glob('pokitto_*')) and not (output / 'src/meta_audio.cpp').exists()
    native, interpreted = host_runs(output, common)
    fp = sg_fingerprint if system == 'sg' else fingerprint
    assert fp(native) == fp(interpreted) == fp(baseline)
    assert int(re.search(r'compiled=(\d+)', native)[1]) > 0
    print(system + ': precise native = interpreter on the ESPboy project')
    pio(output)


def streamed(args, root, host):
    root.mkdir()
    rom = root / 'big.gg'
    data = bytearray(gg_fixture())
    data += random.Random(7).randbytes(20 * 0x4000 - len(data))
    rom.write_bytes(data)
    coverage = root / 'big.cov'
    common = ('--frames', 60, '--input', INPUT)
    baseline = fingerprint(run(host, rom, *common, '--coverage', coverage))
    output = root / 'out'
    generate(args, rom, coverage, output)
    config = (output / 'src/sg_config.h').read_text()
    assert '#define SG_ROM_STREAM 1' in config and 'GG_STREAM_NAME' not in config and 'GG_CART_SD 1' not in config
    assert '#define GG_STREAM_SECTORS 6' in config
    assert (output / 'rom.bin').read_bytes() == bytes(data[0x4000:])  # pages 1 and up, raw
    assert not (output / 'src/gg_stream.c').exists() and not (output / 'src/meta_sd.c').exists()
    native, interpreted = host_runs(output, common)
    assert fingerprint(native) == fingerprint(interpreted) == baseline
    print('gg: a 320 KiB ROM (page 0 in the firmware, the rest in rom.bin) gives the interpreter\'s run')
    pio(output)
    # more than the ROM area holds
    huge = root / 'huge.gg'
    huge.write_bytes(random.Random(8).randbytes(0x400000))
    failed = subprocess.run([args.recompiler, str(huge), '--target', 'espboy', '-o', str(root / 'huge')],
                            text=True, capture_output=True)
    assert failed.returncode and 'ROM area' in failed.stderr, failed.stderr


def cart_ram_flash(args, root, host):
    """espboy_flash.cpp in its Sega build, against the mock flash."""
    root.mkdir()
    rom = root / 'cart.gg'
    data = bytearray(gg_fixture())
    assert data[:3] == bytes.fromhex('f3 ed 56')
    data[:3] = bytes.fromhex('c3 00 3e')
    data[0x3e00:0x3e00 + 20] = bytes.fromhex('f3 3e 08 32 fc ff 3e 5a 32 00 80 af 32 fc ff ed 56 c3 03 00')
    rom.write_bytes(data)
    coverage = root / 'cart.cov'
    run(host, rom, '--frames', 60, '--coverage', coverage)
    assert '# cart-ram' in coverage.read_text()
    output = root / 'out'
    generate(args, rom, coverage, output)
    config = (output / 'src/sg_config.h').read_text()
    assert '#define SG_CART_RAM 1' in config and '#define GG_CART_RAM_8K 1' in config and 'GG_CART_SD 0' in config
    src = output / 'src'
    for c in ('espboy_flash.cpp', 'espboy_sega_flash_check.cpp'):
        source = src / c if c == 'espboy_flash.cpp' else TESTS / c
        run('c++', '-O1', '-w', '-I', TESTS / 'espboy_mock', '-I', src, '-c', source, '-o', root / (c + '.o'))
    run('c++', root / 'espboy_flash.cpp.o', root / 'espboy_sega_flash_check.cpp.o', '-o', root / 'check')
    print(run(root / 'check').strip())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--recompiler', required=True)
    parser.add_argument('--gg-host', required=True)
    parser.add_argument('--sg-host', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='m0_sega_esp_') as temporary:
        root = Path(temporary)
        gg_common = ('--frames', 60, '--input', INPUT)
        small(args, root / 'gg', 'gg', gg_fixture(), gg_common, args.gg_host)
        small(args, root / 'sms', 'sms', gg_fixture(), ('--frames', 60, '--input', '20:128:5,30:16:10,40:8:20'), args.gg_host)
        small(args, root / 'sg', 'sg', sg_fixture(), ('--frames', 24, '--pause-frame', 8), args.sg_host)
        streamed(args, root / 'streamed', args.gg_host)
        cart_ram_flash(args, root / 'cart', args.gg_host)


if __name__ == '__main__':
    main()
