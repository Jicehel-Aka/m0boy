"""Synthetic MSX cartridge (16 KiB at 4000) for the runtime and end-to-end
tests.  It exercises what the simulated BIOS (targets/msx/runtime/msx.c) and
the machine provide: INIGRP, LDIRVM, WRTVRM, FILVRM, GICINI, WRTPSG, RSLREG,
ENASLT, the H.TIMI hook, JIFFY, GTSTCK, GTTRIG, SNSMAT, and the PPI, the AY
registers and the VDP read directly.

No ROM or ROM-derived data: everything is assembled here."""
from z80asm import Asm

SOURCE = r"""
org 0x4000
    db 0x41,0x42           ; the assembler lower-cases quoted text
    dw init
    dw 0
    dw 0
    dw 0
    db 0,0,0,0,0,0

org 0x4010
init:
    ld a,0xc3              ; H.TIMI: JP isr
    ld (0xfd9f),a
    ld hl,isr
    ld (0xfda0),hl
    call 0x0072            ; INIGRP
    ld hl,tile             ; tile 1: pattern and colours (first third of the screen)
    ld de,0x0008
    ld bc,8
    call 0x005c            ; LDIRVM
    ld hl,tile_colour
    ld de,0x2008
    ld bc,8
    call 0x005c
    ld hl,0x1800+33        ; name table (the BIOS default of screen 2): tile 1 here, and a run of them
    ld a,1
    call 0x004d            ; WRTVRM
    ld hl,0x1840
    ld bc,40
    ld a,1
    call 0x0056            ; FILVRM
    ld hl,sprite_pattern   ; sprite 0: a solid 8x8 square
    ld de,0x3800
    ld bc,8
    call 0x005c
    ld hl,sprite_attr
    ld de,0x1b00
    ld bc,8
    call 0x005c
    ld hl,0x1800+33        ; read back through RDVRM
    call 0x004a
    ld (0xe020),a
    call 0x0090            ; GICINI
    ld a,7                 ; mixer: tone 0 and 1, no noise
    ld e,0xb8
    call 0x0093
    ld a,0                 ; tone 0 period 0x0fe
    ld e,0xfe
    call 0x0093
    ld a,2                 ; tone 1 period 0x1a0
    ld e,0xa0
    call 0x0093
    ld a,3
    ld e,0x01
    call 0x0093
    ld a,8
    ld e,0x0f
    call 0x0093
    ld a,9                 ; channel 1 on the envelope
    ld e,0x10
    call 0x0093
    ld a,11
    ld e,0x30
    call 0x0093
    ld a,13
    ld e,0x0e              ; /\/\
    call 0x0093
    call 0x0138            ; RSLREG
    ld (0xe010),a
    ld a,1                 ; page 2 on the cartridge slot: a 16 KiB ROM leaves it empty
    ld h,0x80
    call 0x0024            ; ENASLT
    ld a,(0x8000)
    ld (0xe011),a
    ld a,3                 ; and back on the RAM
    ld h,0x80
    call 0x0024
    in a,(0xa8)
    ld (0xe012),a
    ld a,0x8a              ; CALLF-less far call: CALSLT through IX
    ld ix,helper
    ld iy,0
    call 0x001c            ; CALSLT
    ld a,(0xf3e9)          ; the BIOS variables it leaves
    ld (0xe013),a
    ld a,(0x0006)          ; VDP write port
    ld (0xe014),a
    ei
main:
    ld a,(0xe000)
    inc a
    ld (0xe000),a
    ld a,1                 ; stick 1
    call 0x00d5
    ld (0xe004),a
    ld a,1                 ; trigger 1
    call 0x00d8
    ld (0xe005),a
    ld a,8                 ; keyboard row 8 (cursor keys, space)
    call 0x0141
    ld (0xe006),a
    ld a,14                ; joystick port 1 through the PSG
    out (0xa0),a
    in a,(0xa2)
    ld (0xe007),a
    ld a,8                 ; the same row through the PPI
    out (0xaa),a
    in a,(0xa9)
    ld (0xe008),a
    ld a,(0xfc9e)          ; JIFFY moves the sprite
    out (0x99),a
    ld a,0x01              ; sprite 0, x: VRAM 1b01
    out (0x99),a
    ld a,0x5b
    out (0x99),a
    ld a,(0xfc9e)
    out (0x98),a
    ld a,(0xe000)          ; a few operations on the RAM window
    ld hl,0xe100
    ld (hl),a
    ld de,0xe101
    ld bc,15
    ldir
    ld a,(0xe010)
    ld b,a
    ld a,(0xe100)
    add a,b
    ld (0xe018),a
    ld ix,0x1234           ; the interrupt routine must give IX and IY back (the BIOS saves them)
    ld iy,0x5678
    halt
    push ix
    pop hl
    ld (0xe030),hl
    push iy
    pop hl
    ld (0xe032),hl
    jr main

helper:
    ld a,0x5a
    ld (0xe016),a
    ret

isr:
    ld ix,0xdead
    ld iy,0xbeef
    ld a,(0xe002)
    inc a
    ld (0xe002),a
    ld a,(0xf3e7)          ; STATFL
    ld (0xe003),a
    ret

tile:
    db 0xaa,0x55,0xaa,0x55,0xf0,0xf0,0x0f,0x0f
tile_colour:
    db 0xf1,0x1f,0xe4,0x4e,0x8a,0xa8,0x61,0x16
sprite_pattern:
    db 0xff,0x81,0xbd,0xa5,0xa5,0xbd,0x81,0xff
sprite_attr:
    db 60,100,0,8, 0xd0,0,0,0
"""


def fixture():
    return bytes(Asm(0x10000).assemble(SOURCE)[0x4000:0x8000])


if __name__ == '__main__':
    import sys
    open(sys.argv[1], 'wb').write(fixture())


# ---- banked cartridges ------------------------------------------------------
# Unit u (8 KiB) of the ROM starts with `ld a,u; ret`; the code, in unit 0
# (4000-5FFF, which every mapper shows at power on), selects units through each
# mapper's registers and calls into them, keeping the results in RAM.

MAPPERS = {
    # name: (register of the window 8000, of A000 (None: the pair of the first), of 6000 (None: always unit 1))
    'konami': (0x8000, 0xa000, 0x6000),
    'konami-scc': (0x9000, 0xb000, 0x7000),
    'ascii8': (0x7000, 0x7800, 0x6800),
    'ascii16': (0x7000, None, None),
}
VALUES = (1, 2, 3, 5, 15, 9, 8)


def mapper_source(name):
    reg4, reg5, reg3 = MAPPERS[name]
    lines = ['org 0x4000', 'db 0x41,0x42', 'dw init', 'dw 0', 'dw 0', 'dw 0', 'db 0,0,0,0,0,0',
             'org 0x4010', 'init:', 'call 0x006f',      # INIT32
             'ld a,1', 'ld h,0x80', 'call 0x0024',  # ENASLT: 8000-BFFF on the cartridge
             'ld hl,0x3800+40', 'ld a,0x41', 'call 0x004d']
    for i, v in enumerate(VALUES):
        lines += [f'ld a,{v}', f'ld ({reg4:#x}),a', 'call 0x8000', f'ld (0xe100+{i}),a']
        if reg5 is not None:
            lines += [f'ld a,{v + 1}', f'ld ({reg5:#x}),a']
        lines += ['call 0xa000', f'ld (0xe110+{i}),a']
    if reg3 is not None:
        lines += ['ld a,3', f'ld ({reg3:#x}),a']
    lines += ['ld a,(0x6000)', 'ld (0xe120),a', 'ei', 'main:', 'ld a,(0xe000)', 'inc a', 'ld (0xe000),a',
              'ld hl,0xe200', 'ld (hl),a', 'ld a,(0x8000)', 'ld (0xe121),a', 'halt', 'jr main']
    return '\n'.join(lines)


def mapper_fixture(name, units=32):
    """a ROM of `units` 8 KiB units for the mapper (FF filler: it packs well)"""
    code = bytes(Asm(0x10000).assemble(mapper_source(name))[0x4000:0x6000])
    rom = bytearray(b'\xff' * (units * 0x2000))
    rom[:0x2000] = code
    for u in range(1, units):
        rom[u * 0x2000:u * 0x2000 + 3] = bytes([0x3e, u, 0xc9])
    return bytes(rom)


def mapper_expected(name, units=32):
    """RAM contents the fixture leaves: {address: value}"""
    expected = {}
    for i, v in enumerate(VALUES):
        low, high = ((2 * v) % units, (2 * v + 1) % units) if name == 'ascii16' else (v % units, (v + 1) % units)
        expected[0xe100 + i] = low
        expected[0xe110 + i] = high
    expected[0xe120] = 0x3e          # the byte at 6000: the first of a unit
    expected[0xe121] = 0x3e          # and the first byte of the unit 8000 shows
    return expected


TEXT_SOURCE = r"""
org 0x4000
    db 0x41,0x42
    dw init
    dw 0
    dw 0
    dw 0
    db 0,0,0,0,0,0
org 0x4010
init:
    call 0x006f            ; INIT32: screen 1 with the BIOS font
    ld hl,msg
next:
    ld a,(hl)
    or a
    jr z,done
    push hl
    call 0x00a2            ; CHPUT
    pop hl
    inc hl
    jr next
done:
    ld hl,0x0503           ; POSIT: column 5, row 3
    call 0x00c6
    ld a,0x5a
    call 0x00a2
    ei
spin:
    halt
    jr spin
msg:
    db 0x48,0x49,13,10,0x4f,0x4b,0
"""


def text_fixture():
    """CHPUT and POSIT in screen 1: "HI", a new line, "OK", then a Z at column 5, row 3"""
    return bytes(Asm(0x10000).assemble(TEXT_SOURCE)[0x4000:0x4100])


SCC_SOURCE = r"""
org 0x4000
    db 0x41,0x42
    dw init
    dw 0
    dw 0
    dw 0
    db 0,0,0,0,0,0
org 0x4010
init:
    ld a,1                 ; 8000-BFFF on the cartridge
    ld h,0x80
    call 0x0024
    ld a,7                 ; AY: tones and noise off, so the SCC is what is heard
    ld e,0xbf
    call 0x0093
    ld a,0x3f
    ld (0x9000),a          ; the bank-3 register's value switches the SCC on
    ld hl,wave
    ld de,0x9800           ; voice 1's waveform
    ld bc,32
    ldir
    ld a,111               ; period 111: 3579545 / (32 * 112) = 998.8 Hz
    ld (0x9880),a
    xor a
    ld (0x9881),a
    ld a,15
    ld (0x988a),a
    ld a,1
    ld (0x988f),a
    ei
spin:
    halt
    jr spin
wave:
    db 100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100
    db -100,-100,-100,-100,-100,-100,-100,-100,-100,-100,-100,-100,-100,-100,-100,-100
"""


def scc_fixture(units=8):
    """a Konami SCC cartridge that plays a square wave on voice 1"""
    code = bytes(Asm(0x10000).assemble(SCC_SOURCE)[0x4000:0x6000])
    rom = bytearray(b'\xff' * (units * 0x2000))
    rom[:0x2000] = code
    return bytes(rom)
