#!/usr/bin/env python3
"""Gamebuino AKA project ownership and host parity; the optional real ESP32 build
runs when AKA_PIO=/path/to/pio (the espressif32 platform installed).
Optional firmware execution: AKA_GM0=/path/to/gm0. Device verification remains manual."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from test_meta_target_e2e import make_rom, run, build_host, parse


def main():
    cli = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='aka_') as tmp:
        root = Path(tmp)
        rom = root / 'synthetic.gb'
        rom.write_bytes(make_rom(1989))
        project = root / 'project'
        base = [cli, str(rom), '--target', 'aka', '--output-prefix', 'e2e',
                '--meta-static', '--meta-precise', '-o', str(project)]
        run(base)
        config = (project / 'src/gbl_config.h').read_text()
        assert '#define GBL_TARGET_AKA 1' in config
        assert '#define GBL_ERAM_ON_SD 0' in config
        assert 'GBL_ROM_SD' not in config
        assert '#define GBL_STATE_NAME ' in config  # the states on the card
        for name in ('esp32_hw.cpp', 'esp32_hw.h', 'esp32_sd.cpp', 'esp32_config.h',
                     'aka_hw.cpp', 'aka_hw.h'):
            assert (project / 'src' / name).is_file(), name
        assert not list((project / 'src').glob('meta_*'))
        assert not list((project / 'src').glob('pokitto_*'))
        assert not list((project / 'src').glob('espboy_*'))
        ini = (project / 'platformio.ini').read_text()
        assert 'board = gamebuino_aka' in ini and 'flash_size = 8MB' in ini
        assert (project / 'aka.csv').is_file()
        # the same engine as the host harness: precise == interpreter
        build_host(project)
        results = [parse(run([str(project / 'host/build/e2e_host'), '--frames', '180', *mode]))
                   for mode in ([], ['--interp'])]
        assert results[0]['run_hash'] == results[1]['run_hash'], results

        pio = os.environ.get('AKA_PIO')
        if pio:
            result = subprocess.run([pio, 'run', '-d', str(project), '-j', '2'], capture_output=True,
                                    text=True, timeout=1800)
            assert result.returncode == 0, result.stdout[-2000:] + result.stderr[-2000:]
            firmware = project / '.pio/build/aka/firmware.bin'
            assert firmware.is_file()
            assert firmware.stat().st_size <= 0x2EE000
            print('aka: real ESP32 firmware built (%d bytes)' % firmware.stat().st_size)
            gm0 = os.environ.get('AKA_GM0')
            if gm0:
                env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', EMU_NOPACE='1')
                shot = project / 'aka.ppm'
                result = subprocess.run([gm0, str(firmware.with_suffix('.elf')), '--frames', '24', '--shot', str(shot)],
                                        capture_output=True, text=True, env=env, timeout=120)
                assert result.returncode == 0, result.stdout + result.stderr
                assert 'AKA:' not in result.stderr, result.stderr
                pixels = shot.read_bytes().split(b'\n255\n')[1]
                assert len(pixels) == 320 * 240 * 3
                assert len(set(pixels[i:i+3] for i in range(0, len(pixels), 3))) > 1, 'blank AKA screen'
                print('aka: native firmware executed and screen drawn in gm0 (timing estimated)')
        else:
            print('aka: generation and host parity checked (AKA_PIO unset: no pio build)')
        # switching the project to another console cleans the ESP32 files
        run([cli, str(rom), '--target', 'pc', '--meta-static', '--meta-precise',
             '--output-prefix', 'e2e', '-o', str(project)])
        assert not (project / 'src/aka_hw.cpp').exists()
        assert not (project / 'src/esp32_hw.cpp').exists()


if __name__ == '__main__':
    main()
