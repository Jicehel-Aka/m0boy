#!/usr/bin/env python3
"""NES end to end: a synthetic NROM cartridge (one striped tile, a blue
backdrop) generated as a host project, the harness runs it twice (same
run_hash) and the picture shows the tile over the backdrop."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GBRECOMP = ROOT / "build" / "bin" / "gbrecomp"

# reset at $8000: no NMI, backdrop $22, tile 1 at $2000, rendering on, idle
PRG = bytes([
    0x78, 0xD8,                                     # SEI CLD
    0xA9, 0x00, 0x8D, 0x00, 0x20,                   # LDA #0 STA $2000
    0xA9, 0x3F, 0x8D, 0x06, 0x20,                   # LDA #$3F STA $2006
    0xA9, 0x00, 0x8D, 0x06, 0x20,                   # LDA #0  STA $2006
    0xA9, 0x22, 0x8D, 0x07, 0x20,                   # LDA #$22 STA $2007
    0xA9, 0x20, 0x8D, 0x06, 0x20,                   # LDA #$20 STA $2006
    0xA9, 0x00, 0x8D, 0x06, 0x20,                   # LDA #0  STA $2006
    0xA9, 0x01, 0x8D, 0x07, 0x20,                   # LDA #1  STA $2007
    0xA9, 0x0A, 0x8D, 0x01, 0x20,                   # LDA #$0A STA $2001
    0x80, 0xFE,                                     # BRA $ (65C02 only!) replaced below
])
PRG = PRG[:-2] + bytes([0x4C, 0x27, 0x80])            # JMP $8027 area? no: idle at the last LDA
# place: idle loop at the STA $2001's end ($8028): JMP $8028
PRG = PRG[:-3] + bytes([0x4C, 0x2A, 0x80])   # JMP $802A (idle, past STA $2001)
PRG = PRG.ljust(32768, b"\xEA")                       # NOP fill
# vectors at the end of the last 16 KiB bank
PRG = PRG[:-6] + bytes([0x00, 0x80, 0x00, 0x80, 0x00, 0x80])  # NMI, RESET, IRQ

CHR = bytearray(8192)
for r in range(8):                                    # tile 1: every row striped
    CHR[16 + r] = 0b01010101                          # lo plane
    CHR[24 + r] = 0b01010101                          # hi plane: colour 3
ROM = bytes([0x4E, 0x45, 0x53, 0x1A, 2, 1, 0x01, 0x00] + [0] * 8) + PRG + bytes(CHR)


def run(*args, ok=True):
    out = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if (out.returncode == 0) != ok:
        print(out.stdout, out.stderr, sep="\n")
        raise SystemExit(f"unexpected return {out.returncode}: {' '.join(str(a) for a in args)}")
    return out.stdout + out.stderr


def main():
    work = Path(tempfile.mkdtemp(prefix="nes_e2e_"))
    rom = work / "synthetic.nes"
    rom.write_bytes(ROM)
    project = work / "proj"

    run(GBRECOMP, "--system", "nes", rom, "--target", "desktop",
        "--output-prefix", "synth", "-o", project)

    build = project / "host" / "build"
    run("cmake", "-G", "Ninja", "-S", project / "host", "-B", build,
        "-DCMAKE_BUILD_TYPE=Release")
    run("ninja", "-C", build)

    host = build / "synth_host"
    out1 = run(host, "--frames", "60")
    out2 = run(host, "--frames", "60")
    h1 = [t for t in out1.split() if t.startswith("run_hash=")][0]
    h2 = [t for t in out2.split() if t.startswith("run_hash=")][0]
    assert h1 == h2 and h1 != "run_hash=00000000", f"unstable hash: {h1} {h2}"

    run(host, "--frames", "60", "--ppm", work / "shot.ppm")
    body = (work / "shot.ppm").read_bytes().split(b"\n255\n", 1)[1]
    px = body[(6 * 256 + 128) * 3:(6 * 256 + 128) * 3 + 3]
    # the backdrop $22 through the blank tile 0: the emulator's blue
    assert abs(px[0] - 104) < 40 and abs(px[2] - 252) < 40, f"backdrop wrong: {tuple(px)}"
    px = body[(0 * 256 + 1) * 3:(0 * 256 + 1) * 3 + 3]
    # the tile sits at nametable entry 0 (screen columns 0-7); its pixel 1
    # is the stripe: colour index 3 of palette 0 = $00, grey — any
    # non-backdrop value proves the tile walk
    assert abs(int(px[0]) - 104) > 40 or abs(int(px[2]) - 252) > 40, f"tile pixel wrong: {tuple(px)}"
    print("ok: NES e2e (stable", h1 + ", backdrop and tile pixels verified)")


if __name__ == "__main__":
    main()
