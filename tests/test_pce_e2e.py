#!/usr/bin/env python3
"""PC Engine end to end: a synthetic HuCard (a striped character over a
green backdrop) generated as a host project, the harness runs it twice
(same run_hash) and the picture shows the character over the backdrop.
The cartridge exercises the boot path of the real cards: the reset vector
in the cartridge's page 0 (MPR7 = $00), the MPR setup through TAM, the
VDC programmed through its register and VRAM ports, the VCE palette."""

import os
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GBRECOMP = ROOT / "build" / "bin" / "gbrecomp"


def s8(v):
    return v - 256 if v > 127 else v


def put(code, at, label):
    return [(at, b) for b in code]


def build_rom():
    # the program at $E000 (the reset vector points here; MPR7 = $00 at
    # reset, so $E000-$FFFF is the cartridge's page 0)
    prog = []
    at = 0xE000

    def emit(*bytes_):
        nonlocal at
        prog.append((at, list(bytes_)))
        at += len(bytes_)

    emit(0x78)                      # SEI
    emit(0xD4)                      # CSH
    emit(0xD8)                      # CLD
    emit(0xA2, 0xFF, 0x9A)          # LDX #$FF; TXS
    emit(0xA9, 0xFF, 0x53, 0x01)    # LDA #$FF; TAM #$01 (MPR0 = $FF, I/O)
    emit(0xA9, 0xF8, 0x53, 0x02)    # LDA #$F8; TAM #$02 (MPR1 = $F8, RAM)

    def vdc_select(reg):
        emit(0xA9, reg, 0x8D, 0x00, 0x00)          # LDA #reg; STA $0000

    def vdc_data(lo, hi):
        emit(0xA9, lo, 0x8D, 0x02, 0x00)           # STA $0002
        emit(0xA9, hi, 0x8D, 0x03, 0x00)           # STA $0003

    def mawr(addr):
        vdc_select(0x00)
        vdc_data(addr & 0xFF, addr >> 8)

    # the display: 256 px wide, 60 lines, BG on
    vdc_select(0x0B); vdc_data(0x1F, 0x04)         # HDR: HDW=31, HDE=4
    vdc_select(0x0A); vdc_data(0x02, 0x00)         # HSR: HSW=2, HDS=0
    vdc_select(0x0D); vdc_data(0x3B, 0x00)         # VDR: VDW=59
    vdc_select(0x0C); vdc_data(0x00, 0x00)         # VSR: VSW=0, VDS=0
    vdc_select(0x0E); vdc_data(0x00, 0x00)         # VCR
    vdc_select(0x09); vdc_data(0x10, 0x00)         # MWR: 64x32 BAT
    vdc_select(0x05); vdc_data(0x80, 0x00)         # CR: BG on

    # BAT (4,4) = character 1, palette 0: $0104
    mawr(0x0104)
    vdc_select(0x02); vdc_data(0x01, 0x00)

    # character 1's planes at words 16-31: plane 0 all ones (color 1)
    mawr(0x0010)
    vdc_select(0x02)
    for _ in range(8):
        vdc_data(0xFF, 0x00)
    mawr(0x0018)
    vdc_select(0x02)
    for _ in range(8):
        vdc_data(0x00, 0x00)

    # the VCE: backdrop (entry 0) green, entry 1 red
    emit(0xA9, 0x00, 0x8D, 0x02, 0x04)             # ctaddress = 0
    emit(0xA9, 0x00, 0x8D, 0x04, 0x04)             # green: G=4 -> $100
    emit(0xA9, 0x01, 0x8D, 0x05, 0x04)
    emit(0xA9, 0x01, 0x8D, 0x02, 0x04)             # ctaddress = 1
    emit(0xA9, 0x38, 0x8D, 0x04, 0x04)             # red: R=7 -> $038
    emit(0xA9, 0x00, 0x8D, 0x05, 0x04)

    idle = at
    emit(0x80, 0xFE)                               # BRA self

    rom = bytearray(0x8000)
    for base, bytes_ in prog:
        for i, b in enumerate(bytes_):
            rom[(base & 0x1FFF) + i] = b   # MPR7 = $00: $E000+ is file 0+
    assert idle == 0xE000 + sum(len(b) for _, b in prog[:-1])
    # the vectors in page 0: $FFF6 BRK/IRQ2, $FFF8 IRQ1, $FFFA TIQ,
    # $FFFC NMI, $FFFE RESET — the first four at a RTI stub
    rti = idle + 2
    rom[rti - 0xE000] = 0x40                       # RTI
    for addr in (0x1FF6, 0x1FF8, 0x1FFA, 0x1FFC):
        rom[addr] = rti & 0xFF
        rom[addr + 1] = rti >> 8
    rom[0x1FFE] = 0x00
    rom[0x1FFF] = 0xE0
    return bytes(rom)


def run(*args, ok=True):
    out = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if (out.returncode == 0) != ok:
        print(out.stdout, out.stderr, sep="\n")
        raise SystemExit(f"unexpected return {out.returncode}: {' '.join(str(a) for a in args)}")
    return out.stdout + out.stderr


def main():
    work = Path(tempfile.mkdtemp(prefix="pce_e2e_"))
    rom = work / "synthetic.pce"
    rom.write_bytes(build_rom())
    project = work / "proj"

    run(GBRECOMP, "--system", "pce", rom, "--target", "desktop",
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

    run(host, "--frames", "60", "--wav", work / "sound.wav")
    with wave.open(str(work / "sound.wav"), "rb") as sound:
        assert sound.getnchannels() == 2 and sound.getsampwidth() == 1
        assert sound.getframerate() == 11025 and sound.getnframes() > 0

    run(host, "--frames", "60", "--ppm", work / "shot.ppm")
    body = (work / "shot.ppm").read_bytes().split(b"\n255\n", 1)[1]
    W = 256
    def px(x, y):
        o = (y * W + x) * 3
        return tuple(body[o:o + 3])
    # the backdrop through the VCE's entry 0: green (G=4 -> 144)
    c = px(0, 0)
    assert abs(c[1] - 144) < 30 and c[0] < 40 and c[2] < 40, f"backdrop wrong: {c}"
    # the character at BAT (4,4): screen x 32-39, y 32-39, color 1 = red
    c = px(35, 35)
    assert abs(c[0] - 252) < 30 and c[1] < 40 and c[2] < 40, f"character wrong: {c}"
    print("ok: PC Engine e2e (stable", h1 + ", backdrop and character pixels verified)")


if __name__ == "__main__":
    main()
