"""PICO-8: a synthetic cartridge compiled to C and run on the host harness.

The cartridge (tables, closures, strings, the drawing functions, a sprite
sheet and a map) is written here, so no cartridge is kept in the repository.
EXPECTED_HASH is what femto8 produces on the same cartridge and the same buttons
(its screen memory after 600 frames, hashed as the harness does: FNV-1a over
the 8 KiB), recorded once: the generated C must give the same picture.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


def toolchain() -> str | None:
    """The Pokitto firmware needs arm-none-eabi-gcc: PATH or PlatformIO's."""
    if shutil.which('arm-none-eabi-gcc'):
        return ''
    pio = Path.home() / '.platformio' / 'packages' / 'toolchain-gccarmnoneeabi' / 'bin'
    return str(pio) + '/' if (pio / 'arm-none-eabi-gcc').exists() else None

FRAMES = 600
INPUT = "30:16:100,200:32:3,300:1:20,350:2:20"
EXPECTED_HASH = "fd15b7d6"

LUA = r'''
-- synthetic test cartridge: tables, closures, strings, drawing
objs={}
frames_seen=0
score=0
function make(x,y,c)
 local o={x=x,y=y,c=c,vx=1,vy=0}
 o.step=function(self)
  self.x+=self.vx
  self.y+=self.vy
  if self.x>120 or self.x<2 then self.vx=-self.vx end
  self.y=(self.y+1)%100
 end
 add(objs,o)
 return o
end
function _init()
 for i=1,12 do make(i*8,i*3,i%15+1) end
 msg="hello"
end
function _update()
 frames_seen+=1
 foreach(objs,function(o) o:step() end)
 if btn(4) then score+=1 end
 if btnp(5) then del(objs,objs[1]) end
 if frames_seen%40==0 then add(objs,{x=rnd(100),y=rnd(60),c=8+rnd(7)\1,vx=-1,vy=0,step=function(s) s.x+=s.vx end}) end
end
function _draw()
 cls(1)
 map(0,0,0,0,16,16)
 for o in all(objs) do
  circfill(o.x,o.y,2,o.c)
  spr(1+o.c%3,o.x,o.y,1,1,o.vx<0)
 end
 rectfill(0,100,127,127,0)
 line(0,100,127,100,7)
 print(msg..":"..score.." t="..flr(time()*10)/10,2,104,7)
 print(sub("abcdef",2,4)..tostr(3.25)..tostr(sin(0.125)),2,112,12)
 pset(64,64,9)
 rect(10,10,30,20,11)
 pal(7,8) spr(4,100,80) pal()
end
'''


def cartridge():
    gfx = [[0] * 128 for _ in range(128)]

    def sprite(n, rows):
        sx, sy = (n % 16) * 8, (n // 16) * 8
        for y, row in enumerate(rows):
            for x, ch in enumerate(row):
                gfx[sy + y][sx + x] = int(ch, 16)

    sprite(1, ["00777700", "07eeee70", "7e8888e7", "7e8bb8e7", "7e8bb8e7", "7e8888e7", "07eeee70", "00777700"])
    sprite(2, ["0cc00cc0", "c77cc77c", "c77cc77c", "0cccccc0", "0c0cc0c0", "0c0cc0c0", "c000000c", "00000000"])
    sprite(3, ["11111111", "12222221", "12333321", "12344321", "12344321", "12333321", "12222221", "11111111"])
    sprite(4, ["77777777", "70000007", "70777707", "70700707", "70700707", "70777707", "70000007", "77777777"])
    cells = [[0] * 128 for _ in range(32)]
    for x in range(16):
        cells[15][x] = cells[0][x] = 3
    for y in range(16):
        cells[y][0] = cells[y][15] = 3
    cells[7][7] = cells[7][8] = 4
    out = ["pico-8 cartridge // http://www.pico-8.com", "version 8", "__lua__", LUA.strip(), "__gfx__"]
    out += ["".join("%x" % v for v in row) for row in gfx[:64]]
    out += ["__gff__", "00" * 128, "00" * 128, "__map__"]
    out += ["".join("%02x" % v for v in row) for row in cells]
    out += ["__sfx__", "__music__", ""]
    return "\n".join(out)


def run(*args):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--recompiler', required=True)
    args = parser.parse_args()
    if not shutil.which('cmake') or not shutil.which('ninja'):
        print('cmake and ninja are needed')
        return 77
    with tempfile.TemporaryDirectory(prefix='m0_pico8_') as temporary:
        root = Path(temporary)
        cart = root / 'synthetic.p8'
        cart.write_text(cartridge())
        info = run(args.recompiler, cart, '--info')
        assert 'system=pico8' in info and 'cpu=lua' in info, info
        # host harness: the picture must be femto8's
        output = root / 'desktop'
        run(args.recompiler, cart, '--target', 'desktop', '-o', output)
        run('cmake', '-G', 'Ninja', '-S', output / 'host', '-B', output / 'host/build')
        run('ninja', '-C', output / 'host/build')
        result = run(output / 'host/build/synthetic_host', '--frames', FRAMES, '--input', INPUT)
        found = re.search(r'screen_hash=([0-9a-f]+)', result).group(1)
        assert found == EXPECTED_HASH, 'screen hash %s, femto8 gives %s' % (found, EXPECTED_HASH)
        # a save state at frame 300, loaded by another run, ends on the same picture
        state = root / 'half.p8state'
        run(output / 'host/build/synthetic_host', '--frames', FRAMES // 2, '--input', INPUT, '--save-state', state)
        result = run(output / 'host/build/synthetic_host', '--frames', FRAMES // 2, '--input', INPUT, '--load-state', state)
        found = re.search(r'screen_hash=([0-9a-f]+)', result).group(1)
        assert found == EXPECTED_HASH, 'screen hash %s after a save state' % found
        # a collection of the garbage collector under stress gives the same picture
        stress = root / 'stress'
        run('cmake', '-G', 'Ninja', '-S', output / 'host', '-B', stress, '-DCMAKE_C_FLAGS=-DP8_GC_STRESS')
        run('ninja', '-C', stress)
        result = run(stress / 'synthetic_host', '--frames', FRAMES, '--input', INPUT)
        found = re.search(r'screen_hash=([0-9a-f]+)', result).group(1)
        assert found == EXPECTED_HASH, 'screen hash %s with a collection at every allocation' % found
        state = root / 'stress.p8state'
        run(stress / 'synthetic_host', '--frames', FRAMES // 2, '--input', INPUT, '--save-state', state)
        result = run(stress / 'synthetic_host', '--frames', FRAMES // 2, '--input', INPUT, '--load-state', state)
        found = re.search(r'screen_hash=([0-9a-f]+)', result).group(1)
        assert found == EXPECTED_HASH, 'screen hash %s after a save state, collector under stress' % found
        # the console project is complete
        meta = root / 'meta'
        run(args.recompiler, cart, '--target', 'meta', '-o', meta)
        for name in ('platformio.ini', 'gamebuino_meta.ld', 'src/p8_main.cpp', 'src/p8_gen.c', 'src/p8_cart.c', 'src/p8_config.h'):
            assert (meta / name).is_file(), name
        if shutil.which('pio'):
            run('pio', 'run', '-d', meta)
            assert (meta / '.pio/build/meta/firmware.bin').is_file()
        # the Pokitto project: the console's drivers and the firmware project
        pokitto = root / 'pokitto'
        run(args.recompiler, cart, '--target', 'pokitto', '-o', pokitto)
        for name in ('CMakeLists.txt', 'pokitto.ld', 'arm-toolchain.cmake', 'checksum.py',
                     'src/p8_main.cpp', 'src/p8_config.h', 'src/pokitto_audio.c', 'src/pokitto_lcd.c',
                     'src/pokitto_p8lcd.c', 'src/pokitto_sd.c', 'src/meta_state.cpp', 'src/meta_menu.cpp'):
            assert (pokitto / name).is_file(), name
        for name in ('meta_lcd.cpp', 'meta_sd_bus.cpp', 'platformio.ini', 'gamebuino_meta.ld'):
            assert not (pokitto / 'src' / name).exists() and not (pokitto / name).exists(), name
        config = (pokitto / 'src/p8_config.h').read_text()
        assert 'P8_STATE_NAME83' in config and 'P8_BIN_NAME83' not in config, config
        if toolchain() is None:
            print('pico8 end to end: ok (firmware skipped: no arm-none-eabi-gcc)')
            return 0
        pio = Path.home() / '.platformio' / 'packages' / 'toolchain-gccarmnoneeabi' / 'bin'
        env = dict(os.environ, TOOLCHAIN=str(pio) + '/')
        for compiler in ('gcc', 'clang'):
            folder = 'build-clang' if compiler == 'clang' else 'build'
            if compiler == 'clang' and not shutil.which('clang'):
                continue
            proc = subprocess.run(['cmake', '-G', 'Ninja', '-S', str(pokitto), '-B', str(pokitto / folder),
                                   '-DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake', '-DSG_COMPILER=' + compiler],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
            assert proc.returncode == 0, proc.stdout[-4000:]
            proc = subprocess.run(['ninja', '-C', str(pokitto / folder)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
            assert proc.returncode == 0, proc.stdout[-4000:]
            fw = (pokitto / folder / 'firmware.bin').read_bytes()
            if len(fw) > 0x39000:
                raise SystemExit('pico8 pokitto firmware is %d bytes, above the loader at 0x39000' % len(fw))
            words = struct.unpack_from('<8I', fw, 0)
            if sum(words) & 0xFFFFFFFF:
                raise SystemExit('pico8 pokitto firmware: vector checksum %#x' % (sum(words) & 0xFFFFFFFF))
            sp, reset = words[0], words[1]
            if not (0x10000100 < sp <= 0x10008000) or not (reset & 1) or reset >= len(fw):
                raise SystemExit('pico8 pokitto firmware: vectors SP %#x, reset %#x' % (sp, reset))
            if b'm0boy-pico8-pokitto ' not in fw:
                raise SystemExit('pico8 pokitto firmware: version string missing')
            print('pico8 pokitto %s firmware: %d bytes, checksum and vectors ok' % (compiler, len(fw)))
    print('pico8 end to end: ok')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
