#!/usr/bin/env python3
"""PC (SDL2) target: project ownership, then the firmware built and run head
first on the CI machine — the first destination whose "device" is the host.
Skips the build when SDL2 is missing (PC_SKIP_BUILD=1 forces the skip)."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from test_meta_target_e2e import make_rom, run
from test_sg1000_e2e import fixture as sg_fixture


def firmware_run(exe, frames, ppm):
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
               M0_FRAMES=str(frames), M0_PPM=str(ppm))
    result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, (result.stdout, result.stderr)
    header, _, body = ppm.read_bytes().partition(b'\n255\n')
    assert header.startswith(b'P6'), header
    pixels = {body[i:i + 3] for i in range(0, len(body), 3)}
    assert len(pixels) > 1, 'blank screen'


def build(project):
    build_dir = project / 'build'
    subprocess.run(['cmake', '-B', str(build_dir), '-S', str(project)], check=True,
                   capture_output=True)
    subprocess.run(['cmake', '--build', str(build_dir), '-j4'], check=True, capture_output=True)
    binaries = [p for p in build_dir.iterdir()
                if p.is_file() and os.access(p, os.X_OK)]
    assert len(binaries) == 1, binaries
    return binaries[0]


def main():
    cli = sys.argv[1]
    root = Path(sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix='pc_'))
    can_build = os.environ.get('PC_SKIP_BUILD') != '1' and (
        shutil.which('pkg-config') is None or
        subprocess.run(['pkg-config', '--exists', 'sdl2']).returncode == 0)
    try:
        gb = root / 'synthetic.gb'
        rom = bytearray(make_rom(1989))
        rom[0x147] = 0x01  # MBC1 without RAM/battery: no save file to wait for
        gb.write_bytes(rom)
        project = root / 'gb_pc'
        run([cli, str(gb), '--target', 'pc', '--meta-static', '--meta-precise',
             '--output-prefix', 'e2e', '-o', str(project)])
        config = (project / 'src/gbl_config.h').read_text()
        assert '#define GBL_TARGET_PC 1' in config
        assert 'GBL_ROM_SD' not in config          # the executable embeds the ROM
        assert '#define GBL_STATE_NAME ' in config  # states on the sd.img card
        assert (project / 'src/pc_hw.c').is_file() and (project / 'src/pc_sd.c').is_file()
        assert not list((project / 'src').glob('meta_*'))
        assert not list((project / 'src').glob('pokitto_*'))
        assert not list((project / 'src').glob('espboy_*'))
        assert (project / 'CMakeLists.txt').is_file()
        # switching the project to another console takes the PC files back
        run([cli, str(gb), '--target', 'meta', '--meta-static', '--meta-precise',
             '--output-prefix', 'e2e', '-o', str(project)])
        assert not (project / 'src/pc_hw.c').exists()
        assert not (project / 'CMakeLists.txt').exists()
        run([cli, str(gb), '--target', 'pc', '--meta-static', '--meta-precise',
             '--output-prefix', 'e2e', '-o', str(project)])

        sg = root / 'synthetic.sg'
        sg.write_bytes(sg_fixture())
        sg_project = root / 'sg_pc'
        run([cli, str(sg), '--system', 'sg1000', '--target', 'pc', '-o', str(sg_project)])
        sg_config = (sg_project / 'src/sg_config.h').read_text()
        assert '#define PC_COLS 256' in sg_config and '#define PC_ROWS 192' in sg_config, sg_config
        assert '#define SG_OUT_WIDTH 256' in sg_config
        assert '#define GG_STATE_SLOTS 10' in sg_config

        if can_build:
            exe = build(project)
            firmware_run(exe, 120, root / 'gb.ppm')
            header = (root / 'gb.ppm').read_bytes().split(b'\n')[1]
            assert header == b'160 128', header  # the META view, columns intact
            exe = build(sg_project)
            firmware_run(exe, 120, root / 'sg.ppm')
            header = (root / 'sg.ppm').read_bytes().split(b'\n')[1]
            assert header == b'256 192', header  # the TMS9918A's own screen
            print('pc: GB and SG-1000 firmwares built and run head first')
        else:
            print('pc: SDL2 absent, generation checks only')
    finally:
        if len(sys.argv) < 3:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    main()
