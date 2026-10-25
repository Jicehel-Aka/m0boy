#!/usr/bin/env python3
"""NES end to end, on synthetic NROM cartridges.

1. The picture: one striped tile over a blue backdrop, sprite 0 over it,
   generated as an interpreter-only host project; two runs give the same
   run_hash, the capture shows the tile over the backdrop, and the panel
   renderer (160x128) equals the reference lines' box filter on every row
   with the same sprite-0 hits, every frame shown or one in two left out.
2. The recompiler: a program that exercises the documented instructions
   (ADC/SBC, shifts, BIT, page-crossing indexed reads, (zp),Y and (zp,X),
   JSR/RTS, JMP through RAM, PHP/PLP, the NMI and RTI, OAM DMA, a routine
   copied to RAM and run by the interpreter).  A --meta-precise project
   must print the interpreter's run_hash, from the static walk alone and
   with the harness's own coverage profile; the fast mode must run."""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GBRECOMP = ROOT / "build" / "bin" / "gbrecomp"

# reset at $8000: no NMI, backdrop $22, tile 1 at $2000, sprite 0 over it,
# rendering on, idle
PRG = bytes([
    0x78, 0xD8,                                     # SEI CLD
    0xA9, 0x00, 0x8D, 0x00, 0x20,                   # LDA #0 STA $2000
    0xA9, 0x3F, 0x8D, 0x06, 0x20,                   # LDA #$3F STA $2006
    0xA9, 0x00, 0x8D, 0x06, 0x20,                   # LDA #0  STA $2006
    0xA9, 0x22, 0x8D, 0x07, 0x20,                   # LDA #$22 STA $2007
    0xA9, 0x20, 0x8D, 0x06, 0x20,                   # LDA #$20 STA $2006
    0xA9, 0x00, 0x8D, 0x06, 0x20,                   # LDA #0  STA $2006
    0xA9, 0x01, 0x8D, 0x07, 0x20,                   # LDA #1  STA $2007
    0xA9, 0x00, 0x8D, 0x03, 0x20,                   # LDA #0  STA $2003: sprite 0 ...
    0xA9, 0x02, 0x8D, 0x04, 0x20,                   # y 2 (lines 3-10)
    0xA9, 0x01, 0x8D, 0x04, 0x20,                   # tile 1, over the background's tile 1
    0xA9, 0x00, 0x8D, 0x04, 0x20,                   # in front, palette 4
    0xA9, 0x04, 0x8D, 0x04, 0x20,                   # x 4: sprite 0 hits
    0xA9, 0x1E, 0x8D, 0x01, 0x20,                   # LDA #$1E STA $2001 (both layers, both left columns)
    0x4C, 0x43, 0x80,                               # JMP $8043 (idle, past STA $2001)
]).ljust(32768, b"\xEA")                            # NOP fill
# vectors at the end of the last 16 KiB bank
PRG = PRG[:-6] + bytes([0x00, 0x80, 0x00, 0x80, 0x00, 0x80])  # NMI, RESET, IRQ

CHR = bytearray(8192)
for r in range(8):                                    # tile 1: every row striped
    CHR[16 + r] = 0b01010101                          # lo plane
    CHR[24 + r] = 0b01010101                          # hi plane: colour 3
ROM = bytes([0x4E, 0x45, 0x53, 0x1A, 2, 1, 0x01, 0x00] + [0] * 8) + PRG + bytes(CHR)

# ---- a two-pass 6502 assembler, just what the exercise program needs ----
OPS = {
    "adc": {"imm": 0x69, "zp": 0x65, "zpx": 0x75, "abs": 0x6D, "absx": 0x7D, "absy": 0x79, "izx": 0x61, "izy": 0x71},
    "sbc": {"imm": 0xE9, "zp": 0xE5, "zpx": 0xF5, "abs": 0xED, "absx": 0xFD, "absy": 0xF9, "izx": 0xE1, "izy": 0xF1},
    "and": {"imm": 0x29, "zp": 0x25, "absx": 0x3D, "izy": 0x31},
    "ora": {"imm": 0x09, "zp": 0x05, "absy": 0x19, "izx": 0x01},
    "eor": {"imm": 0x49, "zp": 0x45, "zpx": 0x55, "abs": 0x4D},
    "cmp": {"imm": 0xC9, "zp": 0xC5, "absx": 0xDD, "izy": 0xD1},
    "cpx": {"imm": 0xE0, "zp": 0xE4}, "cpy": {"imm": 0xC0, "abs": 0xCC},
    "bit": {"zp": 0x24, "abs": 0x2C},
    "lda": {"imm": 0xA9, "zp": 0xA5, "zpx": 0xB5, "abs": 0xAD, "absx": 0xBD, "absy": 0xB9, "izx": 0xA1, "izy": 0xB1},
    "ldx": {"imm": 0xA2, "zp": 0xA6, "zpy": 0xB6, "abs": 0xAE, "absy": 0xBE},
    "ldy": {"imm": 0xA0, "zp": 0xA4, "zpx": 0xB4, "abs": 0xAC, "absx": 0xBC},
    "sta": {"zp": 0x85, "zpx": 0x95, "abs": 0x8D, "absx": 0x9D, "absy": 0x99, "izx": 0x81, "izy": 0x91},
    "stx": {"zp": 0x86, "zpy": 0x96, "abs": 0x8E}, "sty": {"zp": 0x84, "zpx": 0x94, "abs": 0x8C},
    "asl": {"acc": 0x0A, "zp": 0x06, "absx": 0x1E}, "lsr": {"acc": 0x4A, "zp": 0x46, "zpx": 0x56},
    "rol": {"acc": 0x2A, "zp": 0x26, "abs": 0x2E}, "ror": {"acc": 0x6A, "zp": 0x66, "absx": 0x7E},
    "inc": {"zp": 0xE6, "zpx": 0xF6, "abs": 0xEE}, "dec": {"zp": 0xC6, "absx": 0xDE},
    "jmp": {"abs": 0x4C, "ind": 0x6C}, "jsr": {"abs": 0x20},
}
IMPLIED = {"inx": 0xE8, "iny": 0xC8, "dex": 0xCA, "dey": 0x88, "tax": 0xAA, "tay": 0xA8, "txa": 0x8A,
           "tya": 0x98, "tsx": 0xBA, "txs": 0x9A, "pha": 0x48, "php": 0x08, "pla": 0x68, "plp": 0x28,
           "clc": 0x18, "sec": 0x38, "cli": 0x58, "sei": 0x78, "clv": 0xB8, "cld": 0xD8, "sed": 0xF8,
           "rts": 0x60, "rti": 0x40, "nop": 0xEA}
BRANCH = {"bpl": 0x10, "bmi": 0x30, "bvc": 0x50, "bvs": 0x70, "bcc": 0x90, "bcs": 0xB0, "bne": 0xD0, "beq": 0xF0}


def assemble(source, origin=0x8000):
    lines = [l.split(";")[0].strip() for l in source.strip().splitlines()]
    labels = {}
    for final in (False, True):
        pc, out = origin, bytearray()

        def value(text):
            text = text.strip()
            if text.startswith("<"):
                return value(text[1:]) & 0xFF
            if text.startswith(">"):
                return value(text[1:]) >> 8
            if text.startswith("$"):
                return int(text[1:], 16)
            if text[0].isdigit():
                return int(text)
            return labels.get(text, 0) if not final else labels[text]

        for line in lines:
            if not line:
                continue
            if line.endswith(":"):
                labels[line[:-1]] = pc
                continue
            op, _, arg = line.partition(" ")
            arg = arg.replace(" ", "")
            if op == ".byte":
                code = bytes(value(v) for v in arg.split(","))
            elif op in IMPLIED:
                code = bytes([IMPLIED[op]])
            elif op in BRANCH:
                off = (value(arg) - (pc + 2)) if final else 0
                assert -128 <= off <= 127, line
                code = bytes([BRANCH[op], off & 0xFF])
            elif arg in ("", "a"):
                code = bytes([OPS[op]["acc"]])
            elif arg.startswith("#"):
                code = bytes([OPS[op]["imm"], value(arg[1:])])
            elif arg.startswith("(") and arg.endswith(",x)"):
                code = bytes([OPS[op]["izx"], value(arg[1:-3])])
            elif arg.startswith("(") and arg.endswith("),y"):
                code = bytes([OPS[op]["izy"], value(arg[1:-3])])
            elif arg.startswith("("):
                v = value(arg[1:-1])
                code = bytes([OPS[op]["ind"], v & 0xFF, v >> 8])
            else:
                base, _, index = arg.partition(",")
                v = value(base)
                zp = (base.startswith("$") and len(base) <= 3) and op not in ("jmp", "jsr")
                mode = ("zp" if zp else "abs") + index
                if mode not in OPS[op] and zp:
                    mode = "abs" + index
                if mode.startswith("zp"):
                    code = bytes([OPS[op][mode], v])
                else:
                    code = bytes([OPS[op][mode], v & 0xFF, v >> 8])
            out += code
            pc += len(code)
    return bytes(out), labels


EXERCISE = """
reset:
    sei
    cld
    ldx #$FF
    txs
    lda #$F0          ; ($20),Y and ($22,X) pointers
    sta $20
    lda #$02
    sta $21
    lda #$80
    sta $23
    lda #$E6          ; INC $1C / RTS, copied to RAM: interpreted
    sta $0500
    lda #$1C
    sta $0501
    lda #$60
    sta $0502
    lda #$80          ; NMI on
    sta $2000
    lda #$1E
    sta $2001
loop:
    inc $10
    lda $10
    clc
    adc $11
    sta $11
    sec
    sbc #$37
    eor $12
    sta $12
    rol a
    ror $13
    asl $14
    lsr $15
    rol $0315
    bit $12
    bvc novf
    inc $16
novf:
    bmi neg
    dec $0316,x
neg:
    ldx $10
    lda table,x       ; crosses a page once X passes $0F
    sta $0300,x
    ldy $11
    lda ($20),y       ; crosses when Y passes $0F
    adc $0300,y
    sta ($20),y
    lda ($22,x)
    and table,x
    ora $0310,y
    cmp ($20),y
    ror $0320,x
    asl $0330,x
    jsr sub
    php
    pla
    sta $17
    pha
    plp
    lda #<back        ; JMP through RAM
    sta $40
    lda #>back
    sta $41
    cpx $11
    cpy #$80
    bcc low
    inc $18
low:
    jmp ($0040)
back:
    dex
    iny
    tya
    tax
    tsx
    txs
    jsr $0500         ; interpreted, returns into compiled code
    lda $10
    and #$07
    bne loop
    lda $2002         ; a register read with a side effect
    sta $1D
    sed               ; decimal is ignored by the 2A03
    lda #$09
    adc #$01
    cld
    sta $1E
    jmp loop
sub:
    lda $12
    and #$0F
    ora #$40
    tay
    dec $19
    bne subdone
    inc $1A
subdone:
    rts
nmi:
    pha
    txa
    pha
    inc $1B
    lda #$03          ; OAM DMA from $0300: the CPU stalls
    sta $4014
    lda $1B
    sta $2005
    sta $2005
    pla
    tax
    pla
    rti
irq:
    rti
table:
"""


def exercise_rom():
    code, labels = assemble(EXERCISE)
    table = bytes((i * 37 + 11) & 0xFF for i in range(256))
    prg = (code + table).ljust(32768, b"\xEA")
    vectors = bytes([labels["nmi"] & 0xFF, labels["nmi"] >> 8, labels["reset"] & 0xFF, labels["reset"] >> 8,
                     labels["irq"] & 0xFF, labels["irq"] >> 8])
    prg = prg[:-6] + vectors
    return bytes([0x4E, 0x45, 0x53, 0x1A, 2, 1, 0x01, 0x00] + [0] * 8) + prg + bytes(CHR)


def run(*args, ok=True):
    out = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if (out.returncode == 0) != ok:
        print(out.stdout, out.stderr, sep="\n")
        raise SystemExit(f"unexpected return {out.returncode}: {' '.join(str(a) for a in args)}")
    return out.stdout + out.stderr


def run_hash(text):
    return [t for t in text.split() if t.startswith("run_hash=")][0]


def panel_ok(text, rows):
    """the harness's panel check: every row equal to the reference's"""
    fields = dict(t.split("=", 1) for t in text.split() if "=" in t)
    assert fields.get("out_mismatch") == "0" and fields.get("hit_mismatch") == "0", text
    assert int(fields["panel_rows"]) == rows, text


def project(rom, out, *options):
    run(GBRECOMP, "--system", "nes", rom, "--target", "desktop", "--output-prefix", "synth", "-o", out, *options)
    build = out / "host" / "build"
    run("cmake", "-G", "Ninja", "-S", out / "host", "-B", build, "-DCMAKE_BUILD_TYPE=Release")
    run("ninja", "-C", build)
    return build / "synth_host"


def picture(work):
    rom = work / "synthetic.nes"
    rom.write_bytes(ROM)
    host = project(rom, work / "proj", "--nes-interp")
    out = run(host, "--frames", "60")
    panel_ok(out, 60 * 128)
    panel_ok(run(host, "--frames", "60", "--skip", "2"), 30 * 128)
    h1 = run_hash(out)
    h2 = run_hash(run(host, "--frames", "60"))
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
    return h1


def recompiler(work):
    rom = work / "exercise.nes"
    rom.write_bytes(exercise_rom())
    frames = ("--frames", "120")
    host = project(rom, work / "precise", "--meta-precise")
    reference = run_hash(run(host, *frames, "--interp"))
    out = run(host, *frames)
    panel_ok(out, 120 * 128)
    native = run_hash(out)
    assert native == reference, f"precise static walk {native} != interpreter {reference}"
    coverage = work / "exercise.cov"
    run(host, *frames, "--coverage", coverage)
    assert " d" in coverage.read_text(), "no dynamic entry in the coverage file"
    host = project(rom, work / "profiled", "--meta-precise", "--coverage", coverage)
    native = run_hash(run(host, *frames))
    assert native == reference, f"precise with coverage {native} != interpreter {reference}"
    host = project(rom, work / "fast", "--coverage", coverage)
    out = run(host, *frames)
    assert run_hash(out) != "run_hash=00000000", out
    return reference


def main():
    work = Path(tempfile.mkdtemp(prefix="nes_e2e_"))
    h = picture(work)
    r = recompiler(work)
    print("ok: NES e2e (picture stable", h + "; recompiled precise = interpreter", r + ", static and profiled)")


if __name__ == "__main__":
    main()
