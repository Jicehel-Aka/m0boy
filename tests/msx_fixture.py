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
    ld hl,0x3800+33        ; name table: tile 1 here, and a run of them
    ld a,1
    call 0x004d            ; WRTVRM
    ld hl,0x3840
    ld bc,40
    ld a,1
    call 0x0056            ; FILVRM
    ld hl,sprite_pattern   ; sprite 0: a solid 8x8 square
    ld de,0x1800
    ld bc,8
    call 0x005c
    ld hl,sprite_attr
    ld de,0x3b00
    ld bc,8
    call 0x005c
    ld hl,0x3800+33        ; read back through RDVRM
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
    ld a,0x40 + 0x3b
    ld c,0x99
    ld a,0x01              ; sprite 0, x: VRAM 3b01
    out (0x99),a
    ld a,0x7b
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
    halt
    jr main

helper:
    ld a,0x5a
    ld (0xe016),a
    ret

isr:
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
