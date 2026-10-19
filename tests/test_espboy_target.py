#!/usr/bin/env python3
"""ESPboy project ownership, flash layout and precise generated/interpreter parity.
Optional real Xtensa build: ESPBOY_PIO=/path/to/pio (SDK already installed).
Uses only the synthetic cartridge shared with the META end-to-end test.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_meta_target_e2e import make_rom, build_host, run, parse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gbrecomp', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='espboy_') as tmp:
        root = Path(tmp)
        rom = root / 'synthetic.gb'
        rom.write_bytes(make_rom(1989))
        project = root / 'project'
        base = [args.gbrecomp, str(rom), '--target', 'espboy', '--output-prefix', 'e2e',
                '--meta-static', '--meta-precise', '-o', str(project)]
        run(base)
        assert (project / 'rom.bin').read_bytes() == rom.read_bytes()
        run(base + ['--meta-pack'])
        assert (project / 'rom.bin').read_bytes() == rom.read_bytes()
        assert 'GBL_ROM_PACKED' not in (project / 'src/gbl_config.h').read_text()
        assert not list((project / 'src').glob('pokitto_*'))
        assert not (project / 'src/meta_hw.h').exists()
        ini = (project / 'platformio.ini').read_text()
        assert 'maximum_size = 262144' in ini and 'flash_size = 1MB' in ini
        config = (project / 'src/gbl_config.h').read_text()
        assert '#define GBL_TARGET_ESPBOY 1' in config
        assert 'GBL_ROM_SD' not in config and 'GBL_STATE_NAME' not in config
        build = build_host(project)
        results = []
        for interp in [[], ['--interp']]:
            result = run([str(build / 'e2e_host'), '--frames', '180', *interp])
            results.append(parse(result)['run_hash'])
        assert results[0] == results[1], results
        print('ESPboy precise compiled/interpreter:', results[0])
        pio = os.environ.get('ESPBOY_PIO')
        if pio:
            run([pio, 'run', '-d', str(project)])
            binary = project / '.pio/build/espboy/firmware.bin'
            assert binary.stat().st_size <= 262144
            print('ESPboy Xtensa firmware:', binary.stat().st_size, 'bytes')
        else:
            print('SKIP Xtensa firmware: set ESPBOY_PIO with an installed ESP8266 SDK')
        # A 1 MiB cartridge needs partial packing in the 748 KiB ROM area.
        large = bytearray(rom.read_bytes())
        large.extend(bytes(1024 * 1024 - len(large)))
        large[0x147] = 0x1b  # MBC5 + RAM + battery, every page selectable
        large[0x148] = 5
        large[0x14d] = (-sum(large[0x134:0x14d]) - 25) & 255
        rom.write_bytes(large)
        run(base)
        config = (project / 'src/gbl_config.h').read_text()
        assert '#define GBL_ROM_PACKED 1' in config and 'GBL_ROM_SD 1' not in config
        assert (project / 'rom.bin').stat().st_size <= 0xbb000
        # Read every logical cartridge byte through the actual runtime cache,
        # including raw sectors, LZSS back-references, and bank transitions.
        (project / 'host/rom_check.c').write_text(r'''#include "gbl.h"
#include <stdio.h>
int gbl_platform_line_check(uint8_t y, uint32_t s) { return 0; }
uint16_t *gbl_platform_line_begin(uint8_t y) { return 0; }
void gbl_platform_line_end(uint8_t y) {}
void gbl_platform_vblank(void) {}
int gbl_platform_save_read(uint32_t s, uint8_t *p) { return 0; }
int gbl_platform_save_write(uint32_t s, const uint8_t *p) { return 0; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    gbl_reset();
    for (unsigned bank = 0; bank < 64; bank++) {
        gbl_wr(0x2000, bank);
        gbl_wr(0x3000, bank >> 8);
        for (unsigned a = 0; a < 16384; a++) {
            int expected = fgetc(f);
            if (expected < 0 || gbl_rd(0x4000 + a) != expected) return 2;
        }
    }
    fclose(f);
    return 0;
}
''')
        with (project / 'host/CMakeLists.txt').open('a') as f:
            f.write('\nadd_executable(rom_check rom_check.c ${GBL_RUNTIME} ${GBL_GENERATED})\n'
                    'target_include_directories(rom_check PRIVATE ${GBL_SRC})\n')
        build = build_host(project)
        run([str(build / 'rom_check'), str(rom)])
        packed_native = parse(run([str(build / 'e2e_host'), '--frames', '180']))
        packed_interp = parse(run([str(build / 'e2e_host'), '--frames', '180', '--interp']))
        assert packed_native['run_hash'] == packed_interp['run_hash']
        assert packed_native['wram_hash'] == packed_interp['wram_hash']
        import re
        offsets = re.search(r'gbl_rom_packed_off\[\].*?= \{(.*?)\}',
                            (project / 'src/e2e_rom.c').read_text(), re.S).group(1)
        offsets = [int(n) for n in re.findall(r'(\d+)u', offsets)]
        packed = sum(0 < b - a < 512 for a, b in zip(offsets, offsets[1:]))
        raw = sum(b - a == 512 for a, b in zip(offsets, offsets[1:]))
        assert packed and raw, (packed, raw)
        print('ESPboy partial packing:', packed, 'compressed sectors,', raw, 'raw; all bytes verified')
        if pio:
            run([pio, 'run', '-d', str(project)])
            assert (project / '.pio/build/espboy/firmware.bin').stat().st_size <= 262144
        import random
        noise = bytearray(random.Random(8086).randbytes(len(large)))
        noise[:512] = large[:512]
        rejected = root / 'incompressible.gb'
        rejected.write_bytes(noise)
        result = subprocess.run([args.gbrecomp, str(rejected), '--target', 'espboy', '-o', str(root / 'rejected')], text=True, capture_output=True)
        assert result.returncode and 'even compressed' in result.stderr, result.stderr
        # Reject SD and architecture-specific paths rather than producing unusable projects.
        for flags in [['--meta-stream', 'on'], ['--meta-thumb']]:
            result = subprocess.run(base + flags, text=True, capture_output=True)
            assert result.returncode and 'ESPboy' in result.stderr
        # Switching consoles must not leave any ESPboy driver or linker script.
        run([args.gbrecomp, str(rom), '--target', 'meta', '-o', str(project)])
        assert not list((project / 'src').glob('espboy_*'))
        assert not (project / 'espboy.ld').exists()
        assert not (project / 'rom.bin').exists()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
