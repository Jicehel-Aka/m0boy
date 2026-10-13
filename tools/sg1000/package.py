#!/usr/bin/env python3
"""Build and package SG-1000 players for Linux and Windows x86-64.

Requires a native SDL2 install prefix, the SDL2 MinGW development archive,
and llvm-mingw. Dependencies stay outside the source tree's tracked files.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import zipfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdl-linux-prefix', type=Path, required=True)
parser.add_argument('--sdl-mingw-root', type=Path, required=True)
parser.add_argument('--mingw-root', type=Path, required=True)
parser.add_argument('--frontend-root', type=Path, default=root.parent/'gm0', help='Unified gm0 frontend source')
parser.add_argument('--zlib-linux-prefix', type=Path, required=True)
parser.add_argument('--zlib-mingw-prefix', type=Path, required=True)
parser.add_argument('--zlib-license', type=Path, required=True)
parser.add_argument('--linux-sysroot', type=Path, help='Older glibc sysroot for portable Linux builds')
parser.add_argument('--rom', type=Path, help='Embed this user-provided cartridge; Windows also embeds SDL2')
parser.add_argument('--name', default='sg1000', help='Distribution and executable name')
parser.add_argument('--output', type=Path, default=root/'output/sg1000-release')
args = parser.parse_args()
if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
    parser.error('--name must contain only letters, digits, underscores and hyphens')
rom = args.rom.read_bytes() if args.rom else None
if rom is not None and not 0 < len(rom) <= 0xc000:
    parser.error('Embedded cartridge must contain 1–49152 bytes')
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
version = (root/'targets/meta/VERSION').read_text().strip()
sdl_linux = args.sdl_linux_prefix.resolve()
sdl_windows = args.sdl_mingw_root.resolve()
mingw = args.mingw_root.resolve()
frontend = args.frontend_root.resolve()
if not (frontend/'cores/sg1000/sg1000.c').is_file():
    parser.error('--frontend-root must point to gm0 with direct SG-1000 support')

def run(*cmd):
    subprocess.run(list(map(str,cmd)),check=True)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

packages = []
for platform in ('linux','windows'):
    name = f'{args.name}-{version}-{platform}-x86_64'
    package = output/name
    if package.exists():
        # Remove only this script's previous staging output.
        if not (package/'manifest.json').is_file() and not (package/'.package-staging').is_file():
            raise SystemExit(f'Refusing to replace unrecognized directory: {package}')
        shutil.rmtree(package)
    package.mkdir()
    (package/'.package-staging').write_text('gm0 SG-1000 package staging\n')
    build = output/('build-'+platform+'-gm0')
    build.mkdir(exist_ok=True)
    flags = ['-DCMAKE_BUILD_TYPE=Release','-DGM0_FR=ON',
             f'-DGM0_WIN_STATIC={"ON" if rom is not None else "OFF"}',
             f'-DGM0_SG_ROM={args.rom.resolve() if args.rom else ""}']
    if platform=='windows':
        flags += ['-DCMAKE_SYSTEM_NAME=Windows','-DCMAKE_SYSTEM_PROCESSOR=x86_64',
                 f'-DCMAKE_C_COMPILER={mingw}/bin/x86_64-w64-mingw32-gcc',
                 f'-DSDL2_DIR={sdl_windows}/lib/cmake/SDL2']
        zlib = args.zlib_mingw_prefix.resolve()
        flags += [f'-DZLIB_INCLUDE_DIR={zlib}/include',f'-DZLIB_LIBRARY={zlib}/lib/libzlibstatic.a']
    else:
        flags += [f'-DSDL2_DIR={sdl_linux}/lib/cmake/SDL2']
        zlib = args.zlib_linux_prefix.resolve()
        flags += [f'-DZLIB_INCLUDE_DIR={zlib}/include',f'-DZLIB_LIBRARY={zlib}/lib/libz.a',
                  '-DCMAKE_INSTALL_RPATH=$ORIGIN/lib']
        if args.linux_sysroot:
            sysroot = args.linux_sysroot.resolve()
            flags += [f'-DCMAKE_C_FLAGS=--sysroot={sysroot} -isystem {sysroot}/usr/include -isystem {sysroot}/usr/include/x86_64-linux-gnu -idirafter /usr/include',
                      f'-DCMAKE_EXE_LINKER_FLAGS=-L{sysroot}/usr/lib/x86_64-linux-gnu -L{sysroot}/lib/x86_64-linux-gnu -Wl,-rpath-link={sysroot}/lib/x86_64-linux-gnu']
    run('cmake','-G','Ninja','-S',frontend,'-B',build,*flags)
    run('ninja','-C',build,'-j','4')
    run('cmake','--install',build,'--prefix',package)
    installed = package/'bin'/('gm0.exe' if platform=='windows' else 'gm0')
    if platform=='windows':
        if rom is None:
            shutil.copy2(sdl_windows/'bin/SDL2.dll',package/'SDL2.dll')
        executable = 'sg1000_play.exe' if rom is None else args.name+'.exe'
        installed.rename(package/executable)
        run(mingw/'bin/x86_64-w64-mingw32-strip',package/executable)
    else:
        (package/'lib').mkdir()
        shutil.copy2((sdl_linux/'lib/libSDL2-2.0.so.0').resolve(),package/'lib/libSDL2-2.0.so.0')
        executable = 'sg1000-play' if rom is None else args.name
        installed.rename(package/executable)
        run('strip',package/executable)
        (package/'play.sh').write_text(f'#!/bin/sh\nset -eu\ncd -- "$(dirname -- "$0")"\nexec ./{executable} "$@"\n')
        (package/'play.sh').chmod(0o755)
    (package/'bin').rmdir()
    licenses = package/'licenses'
    licenses.mkdir()
    shutil.copy2(root/'LICENSE',licenses/'m0boy-MIT.txt')
    shutil.copy2(frontend/'LICENSE',licenses/'gm0-MIT.txt')
    shutil.copy2(args.zlib_license,licenses/'zlib.txt')
    shutil.copy2(args.sdl_mingw_root.resolve().parents[0]/'LICENSE.txt',licenses/'SDL2-zlib.txt')
    if platform=='windows':
        shutil.copy2(mingw/'LICENSE.TXT',licenses/'LLVM.txt')
        for path in (mingw/'x86_64-w64-mingw32/share/mingw32').glob('COPYING*'):
            shutil.copy2(path,licenses/path.name)
    minimum_glibc = None
    if platform=='linux':
        versions = []
        for path in (package/executable,package/'lib/libSDL2-2.0.so.0'):
            text = subprocess.check_output(['readelf','--version-info',str(path)],text=True)
            versions += [tuple(map(int,v.split('.'))) for v in re.findall(r'GLIBC_([0-9.]+)',text)]
        minimum_glibc = '.'.join(map(str,max(versions)))
    launch = f'Ouvrir {executable}' if platform=='windows' else f'Exécuter ./play.sh (ou ./{executable})'
    cartridge = 'Le jeu intégré démarre directement.' if rom is not None else 'Déposer une cartouche .sg dans la fenêtre. Aucune ROM fournie.'
    requirement = 'Windows 10/11 64 bits.' if platform=='windows' else f'Linux x86-64, glibc {minimum_glibc} ou supérieure, X11/Wayland et audio du système.'
    (package/'README.txt').write_text(f'''SG-1000 {version} — {platform} x86-64

{launch}. {cartridge}
Un chemin de cartouche peut aussi être passé en argument.
{requirement}
SDL2 est inclus.

Flèches : déplacement. Espace : bouton 1 (A). Ctrl : bouton 2 (B).
Entrée : Start (bouton 1 sur SG-1000). * : Select (Pause SG-1000).
Échap : quitter.
F5 : redémarrer. F8 : filtres. F10 : échelle. F11 : plein écran.
Les manettes utilisent les mêmes commandes que gm0 META/Pokitto.
Actuel : vitesse réellement tenue, affichage et attente compris.
Possible : capacité du noyau et du son hors affichage/attente, sans plafond.
100 % = cadence NTSC normale (~60 images/s).

Source : gm0 (frontal commun META/Pokitto/SG-1000), noyau SG-1000 de m0boy.
Build : m0boy/tools/sg1000/package.py, CMake et Ninja.
SDL2 : https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10
Licences et notices dans licenses/.
Options : --frames N ; --shot image.ppm ; --wav son.wav.
Cartouches sans mapper, au plus 48 Kio. PAL et extensions non pris en charge.
''')
    (package/'.package-staging').unlink()
    files = {str(path.relative_to(package)):digest(path) for path in sorted(package.rglob('*')) if path.is_file()}
    manifest = {'version':version,'platform':platform,'architecture':'x86_64','sdl_version':'2.32.10',
                'minimum_glibc':minimum_glibc,'embedded_rom_sha256':hashlib.sha256(rom).hexdigest() if rom is not None else None,
                'static_sdl':platform=='windows' and rom is not None,'files_sha256':files}
    (package/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    if platform=='windows':
        archive = output/(name+'.zip')
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as zipout:
            for path in sorted(package.rglob('*')):
                if path.is_file():zipout.write(path,str(path.relative_to(output)))
    else:
        archive = output/(name+'.tar.gz')
        with tarfile.open(archive,'w:gz') as tarout:tarout.add(package,arcname=name)
    packages.append(archive)
(output/'SHA256SUMS').write_text(''.join(f'{digest(path)}  {path.name}\n' for path in packages))
for path in packages:print(path)
