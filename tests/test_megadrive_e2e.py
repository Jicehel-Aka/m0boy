#!/usr/bin/env python3
"""Small libre Mega Drive colour-loop ROM: compile to C and compare with
the independent opcode-decoding reference. The commercial ROM profile is
intentionally outside this first bounded subset."""
import re
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def rom_image():
    rom = bytearray(0x300)
    rom[0:4] = (0x00fff000).to_bytes(4, "big")
    rom[4:8] = (0x200).to_bytes(4, "big")
    rom[0x100:0x10c] = b"SEGA GENESIS"
    pc = 0x200
    labels = {}
    branches = []

    def emit(*words):
        nonlocal pc
        for word in words:
            rom[pc:pc + 2] = word.to_bytes(2, "big")
            pc += 2

    def long(value):
        emit(value >> 16, value & 0xffff)

    def branch(op, label):
        nonlocal pc
        at = pc
        emit(op, 0)
        branches.append((at, label))

    emit(0x33fc, 0x8f02)  # VDP auto increment 2
    long(0xc00004)
    emit(0x7000)          # MOVEQ #0,D0: palette colour
    labels["outer"] = pc
    emit(0x23fc)          # CRAM write address 0
    long(0xc0000000)
    long(0xc00004)
    emit(0x33c0)          # MOVE.W D0,$C00000
    long(0xc00000)
    emit(0x7200)          # MOVEQ #0,D1: delay
    labels["wait"] = pc
    emit(0x5241)          # ADDQ.W #1,D1
    emit(0x0c41, 0x0200)  # CMPI.W #512,D1
    branch(0x6600, "wait")
    emit(0x0640, 0x0020)  # ADDI.W #$20,D0
    emit(0x0c40, 0x00e0)  # CMPI.W #$e0,D0
    branch(0x6600, "outer")
    emit(0x7000)
    branch(0x6000, "outer")
    for at, label in branches:
        delta = labels[label] - (at + 2)
        rom[at + 2:at + 4] = (delta & 0xffff).to_bytes(2, "big")
    return bytes(rom)


def tiled_image():
    """A single red pixel in Plane A, written through the VDP ports."""
    rom = bytearray(rom_image())
    pc = 0x200

    def move_word(value, address):
        nonlocal pc
        rom[pc:pc + 8] = (0x33fc).to_bytes(2, "big") + value.to_bytes(2, "big") + address.to_bytes(4, "big")
        pc += 8

    def move_long(value, address):
        nonlocal pc
        rom[pc:pc + 10] = (0x23fc).to_bytes(2, "big") + value.to_bytes(4, "big") + address.to_bytes(4, "big")
        pc += 10

    move_word(0x8140, 0xc00004)  # display enabled
    move_word(0x8230, 0xc00004)  # Plane A at $c000
    move_long(0xc0020000, 0xc00004)  # CRAM index 1
    move_word(0x000e, 0xc00000)  # red
    move_long(0x40200000, 0xc00004)  # VRAM tile 1
    move_word(0x1000, 0xc00000)  # red pixel in top-left corner
    move_long(0x40000003, 0xc00004)  # VRAM Plane A name table
    move_word(1, 0xc00000)  # tile 1
    rom[pc:pc + 2] = (0x60fe).to_bytes(2, "big")  # BRA -2
    return bytes(rom)


def run(*args, success=True, env=None):
    result = subprocess.run([str(x) for x in args], text=True, capture_output=True, env=env)
    if (result.returncode == 0) != success:
        raise AssertionError(f"{' '.join(map(str, args))}\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def main():
    exe = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="md_e2e_") as tmp:
        work = Path(tmp)
        rom = work / "colour.md"
        rom.write_bytes(rom_image())
        info = run(exe, rom, "--system", "megadrive", "--info")
        assert "cpu=m68k" in info and "rom_bytes=768" in info
        project = work / "desktop"
        run(exe, rom, "--system", "megadrive", "--target", "desktop", "-o", project)
        run("cmake", "-G", "Ninja", "-S", project, "-B", project / "build")
        run("ninja", "-C", project / "build")
        host = project / "build" / "md_host"
        native = run(host, "--frames", "3")
        interp = run(host, "--interp", "--frames", "3")
        assert re.search(r"fault=0\b", native), native
        assert re.search(r"run_hash=([a-f0-9]+)", native).group(1) == re.search(r"run_hash=([a-f0-9]+)", interp).group(1)
        first = run(host, "--frames", "1", "--ppm", work / "image.ppm")
        assert re.search(r"bg=([a-f0-9]+)", first).group(1) != "0000"
        assert (work / "image.ppm").stat().st_size > 160 * 128 * 3
        pc = work / "pc"
        run(exe, rom, "--system", "megadrive", "--target", "pc", "-o", pc)
        run("cmake", "-G", "Ninja", "-S", pc, "-B", pc / "build")
        run("ninja", "-C", pc / "build")
        pc_env = os.environ.copy()
        pc_env["SDL_VIDEODRIVER"] = "dummy"
        pc_env["SDL_AUDIODRIVER"] = "dummy"
        window = run(pc / "build" / "md_pc", "--frames", "3", env=pc_env)
        window_ref = run(pc / "build" / "md_pc", "--frames", "3", "--interp", env=pc_env)
        for output in (window, window_ref):
            assert re.search(r"fault=0\b", output), output
        assert re.search(r"run_hash=([a-f0-9]+)", window).group(1) == re.search(r"run_hash=([a-f0-9]+)", window_ref).group(1)
        run(pc / "build" / "md_pc", "--frames", "1", "--ppm", work / "pc.ppm", env=pc_env)
        capture = (work / "pc.ppm").read_bytes()
        assert len(capture) > 320 * 224 * 3
        assert any(capture[-320 * 224 * 3:])
        tile_rom = work / "tile.md"
        tile_rom.write_bytes(tiled_image())
        tile_pc = work / "tile_pc"
        run(exe, tile_rom, "--system", "megadrive", "--target", "pc", "-o", tile_pc)
        run("cmake", "-G", "Ninja", "-S", tile_pc, "-B", tile_pc / "build")
        run("ninja", "-C", tile_pc / "build")
        tile_exe = tile_pc / "build" / "md_pc"
        tile_native = run(tile_exe, "--frames", "1", "--ppm", work / "tile.ppm", env=pc_env)
        tile_interp = run(tile_exe, "--interp", "--frames", "1", env=pc_env)
        assert re.search(r"fault=0\b", tile_native), tile_native
        assert re.search(r"run_hash=([a-f0-9]+)", tile_native).group(1) == re.search(r"run_hash=([a-f0-9]+)", tile_interp).group(1)
        tile_capture = (work / "tile.ppm").read_bytes().split(b"\n", 3)[3]
        assert tile_capture[:6] == bytes((255, 0, 0, 0, 0, 0)), tile_capture[:6]
        md = work / "meta"
        run(exe, rom, "--system", "megadrive", "--target", "meta",
            "--meta-precise", "-o", md)
        assert (md / "platformio.ini").is_file()
        bad = bytearray(rom_image())
        bad[0x200:0x202] = b"\x4e\x75"  # RTS is outside the bounded profile.
        broken = work / "unsupported.md"
        broken.write_bytes(bad)
        assert "Unsupported M68k opcode" in run(exe, broken, "--system", "megadrive",
                                                  "--target", "meta", "-o", work / "broken",
                                                  success=False)
        fallback = work / "pc_native"
        run(exe, broken, "--system", "megadrive", "--target", "pc", "-o", fallback)
        assert (fallback / "native-status.txt").read_text() == "native-c\n"
        assert (fallback / "src" / "md_full_code.c").is_file()
        assert not list(fallback.rglob("*libretro*"))
        run("cmake", "-G", "Ninja", "-S", fallback, "-B", fallback / "build")
        run("ninja", "-C", fallback / "build")
        assert (fallback / "build" / "md_pc").is_file()
        print(native.strip())


if __name__ == "__main__":
    main()
