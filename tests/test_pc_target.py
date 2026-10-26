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
from gg_fixture import fixture as gg_fixture
from msx_fixture import fixture as msx_fixture
from test_nes_e2e import ROM as nes_fixture


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
        assert (project / 'src/pc_frontend.c').is_file()
        assert not list((project / 'src').glob('meta_*'))
        assert not list((project / 'src').glob('pokitto_*'))
        assert not list((project / 'src').glob('espboy_*'))
        assert (project / 'CMakeLists.txt').is_file()
        # switching the project to another console takes the PC files back
        run([cli, str(gb), '--target', 'meta', '--meta-static', '--meta-precise',
             '--output-prefix', 'e2e', '-o', str(project)])
        assert not (project / 'src/pc_hw.c').exists()
        assert not (project / 'src/pc_frontend.c').exists()
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

        msx = root / 'synthetic.mx1'
        msx.write_bytes(msx_fixture())
        msx_project = root / 'msx_pc'
        run([cli, str(msx), '--system', 'msx', '--target', 'pc', '-o', str(msx_project)])
        assert '#define SG_MSX_RAM 16384' in (msx_project / 'src/sg_config.h').read_text()

        nes = root / 'synthetic.nes'
        nes.write_bytes(nes_fixture)
        nes_project = root / 'nes_pc'
        run([cli, str(nes), '--system', 'nes', '--target', 'pc', '-o', str(nes_project)])
        assert (nes_project / 'src/pc_hw.c').is_file()

        pico_project = None
        if (Path(__file__).resolve().parents[2] / 'femto8').is_dir():
            from test_pico8_e2e import cartridge
            pico = root / 'synthetic.p8'
            pico.write_text(cartridge())
            pico_project = root / 'pico_pc'
            run([cli, str(pico), '--system', 'pico8', '--target', 'pc', '-o', str(pico_project)])
            assert (pico_project / 'src/pc_hw.c').is_file()

        # the PCE window project: the runtime, the SDL2 shell, the ROM as data
        from test_pce_e2e import build_rom as pce_rom
        pce = root / 'synthetic.pce'
        pce.write_bytes(pce_rom())
        pce_project = root / 'pce_pc'
        run([cli, str(pce), '--system', 'pce', '--target', 'pc', '-o', str(pce_project)])
        assert (pce_project / 'src/pce_pc.c').is_file()
        assert (pce_project / 'src/pce_cart.c').is_file()
        assert 'pkg_check_modules(SDL2' in (pce_project / 'CMakeLists.txt').read_text()

        sega_projects = []
        for system, extension in (('gg', '.gg'), ('sms', '.sms')):
            cart = root / ('synthetic' + extension)
            cart.write_bytes(gg_fixture())
            sega_project = root / (system + '_pc')
            run([cli, str(cart), '--system', system, '--target', 'pc', '-o', str(sega_project)])
            assert (sega_project / 'src/pc_frontend.c').is_file()
            assert (sega_project / 'src/gg_stream.c').is_file()
            sega_projects.append((system, sega_project))

        if can_build:
            exe = build(project)
            firmware_run(exe, 120, root / 'gb.ppm')
            header = (root / 'gb.ppm').read_bytes().split(b'\n')[1]
            assert header == b'160 128', header  # the META view, columns intact
            exe = build(sg_project)
            firmware_run(exe, 120, root / 'sg.ppm')
            header = (root / 'sg.ppm').read_bytes().split(b'\n')[1]
            assert header == b'256 192', header  # the TMS9918A's own screen
            for system, project, dims in (('msx', msx_project, b'256 192'),
                                          ('nes', nes_project, b'160 128')):
                firmware_run(build(project), 60, root / (system + '.ppm'))
                assert (root / (system + '.ppm')).read_bytes().split(b'\n')[1] == dims
            if pico_project:
                firmware_run(build(pico_project), 30, root / 'pico.ppm')
                assert (root / 'pico.ppm').read_bytes().split(b'\n')[1] == b'160 128'
                host_build = pico_project / 'host' / 'build'
                subprocess.run(['cmake', '-G', 'Ninja', '-S', str(pico_project / 'host'), '-B', str(host_build)],
                               check=True, capture_output=True)
                subprocess.run(['ninja', '-C', str(host_build)], check=True, capture_output=True)
                host_ppm = root / 'pico_host.ppm'
                host_exe, = host_build.glob('*_host')
                subprocess.run([str(host_exe), '--frames', '30', '--shot', str(host_ppm)],
                               check=True, capture_output=True)
                screen = (root / 'pico.ppm').read_bytes().split(b'\n255\n', 1)[1]
                crop = b''.join(screen[(y * 160 + 16) * 3:(y * 160 + 144) * 3] for y in range(128))
                native = host_ppm.read_bytes().split(b'\n255\n', 1)[1]
                rgb565 = bytes((c & (252 if i % 3 == 1 else 248)) for i, c in enumerate(native))
                assert crop == rgb565, 'PC PICO-8 frame 30 differs from the host harness'
            exe = build(pce_project)
            env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
            shot = root / 'pce.ppm'
            result = subprocess.run([str(exe), '--frames', '120', '--ppm', str(shot)],
                                    env=env, capture_output=True, text=True, timeout=120)
            assert result.returncode == 0, (result.stdout, result.stderr)
            header, _, body = shot.read_bytes().partition(b'\n255\n')
            assert header == b'P6\n256 60', header  # the synthetic card's VDW+1 lines
            assert len({body[i:i + 3] for i in range(0, len(body), 3)}) > 1
            for system, sega_project in sega_projects:
                firmware_run(build(sega_project), 120, root / (system + '.ppm'))
            print('pc: GB, SG-1000, MSX, NES, PICO-8, Game Gear, SMS and PCE built and run head first')
        else:
            print('pc: SDL2 absent, generation checks only')
    finally:
        if len(sys.argv) < 3:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    main()
