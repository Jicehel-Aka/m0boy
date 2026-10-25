/* The NES panel renderer's loops in Thumb-1 (ppu.c holds their C reference,
 * which the host runs; a firmware built with -DNES_NO_PPU_ASM takes the C).
 *
 * nes_ppu_tiles_asm(tiles, out, expand, fine_y)
 *   33 decoded tiles into one line of a pair buffer: per tile its two
 *   stream words, 8 bytes apart (the other line's words between).
 * nes_ppu_out1_asm(group, out, pair53, misalign)
 * nes_ppu_out2_asm(group, out, pair53, misalign)
 *   32 groups of eight stream pixels -> 160 panel pixels, a row alone
 *   (line 0) or two lines averaged.  group: the word pair holding the first
 *   pixel; misalign: fine x & 3, the first pixel's byte in that word.  The
 *   pair tables are one array: pair44 = pair53 + 2048 bytes.  One copy of
 *   each loop per misalignment, so that every byte load has its offset.
 *
 * Code stays in flash: on the META, code run from SRAM faulted with the CPU
 * overclocked (gbl_profile.h, GBL_PPU_RAMFUNC). */
#if __has_include("nes_config.h")
#include "nes_config.h"
#endif
#ifndef NES_PANEL_SWAP
#define NES_PANEL_SWAP 0
#endif

#if defined(__ARM_ARCH_6M__) && !defined(NES_NO_PPU_ASM)

    .syntax unified
    .thumb

/* pixel k of a group starting at byte m of its word, line r: the stream
 * words of the two lines alternate (ppu.c, ROW_AT) */
#define OFF(m, k, r) (((((m) + (k)) >> 2) << 3) | (((m) + (k)) & 3) | ((r) << 2))

.macro PANEL reg
#if NES_PANEL_SWAP
    rev16 \reg, \reg
#endif
.endm

/* dst = table[pixel a][pixel b] (both pixels are index * 2: byte offsets) */
.macro LOOK dst, tmp, base, offa, offb, table
    ldrb \dst, [\base, #\offa]
    ldrb \tmp, [\base, #\offb]
    lsls \dst, \dst, #5
    adds \dst, \dst, \tmp
    ldrh \dst, [\table, \dst]
.endm

/* out[slot] = the average of two native colours, rounded down, panel order
 * (m0_avg2_panel); r10 holds 0xf7de */
.macro AVGST a, b, t, u, slot
    movs \t, \a
    eors \t, \b
    mov \u, r10
    ands \t, \u
    lsrs \t, \t, #1
    ands \a, \b
    adds \a, \a, \t
    PANEL \a
    strh \a, [r1, #\slot]
.endm

/* one colour in five pixels: the byte of r4, its table diagonal */
.macro FLAT
    lsls r5, r4, #24
    lsrs r5, r5, #24
    lsls r7, r5, #5
    adds r5, r5, r7
    ldrh r5, [r2, r5]
    PANEL r5
    strh r5, [r1, #0]
    strh r5, [r1, #2]
    strh r5, [r1, #4]
    strh r5, [r1, #6]
    strh r5, [r1, #8]
.endm

/* m0_box58_fast of line \r of the group at r3, to panel order */
.macro BOX m, r
    ldrb r4, [r3, #OFF(\m, 1, \r)]
    ldrb r5, [r3, #OFF(\m, 0, \r)]
    lsls r5, r5, #5
    adds r5, r5, r4
    ldrh r5, [r2, r5]
    PANEL r5
    strh r5, [r1, #0]
    ldrb r5, [r3, #OFF(\m, 2, \r)]
    lsls r5, r5, #5
    adds r5, r5, r4
    ldrh r5, [r2, r5]
    PANEL r5
    strh r5, [r1, #2]
    mov r6, r8
    LOOK r5, r4, r3, OFF(\m, 3, \r), OFF(\m, 4, \r), r6
    PANEL r5
    strh r5, [r1, #4]
    ldrb r4, [r3, #OFF(\m, 6, \r)]
    ldrb r5, [r3, #OFF(\m, 5, \r)]
    lsls r5, r5, #5
    adds r5, r5, r4
    ldrh r5, [r2, r5]
    PANEL r5
    strh r5, [r1, #6]
    ldrb r5, [r3, #OFF(\m, 7, \r)]
    lsls r5, r5, #5
    adds r5, r5, r4
    ldrh r5, [r2, r5]
    PANEL r5
    strh r5, [r1, #8]
.endm

/* registers in the loops: r0 the group (advanced past it by the loads),
 * r1 the output, r2 pair53, r3 the group again for byte loads, r8 pair44,
 * r9 the end, r10 0xf7de */

/* a row alone: flat when line 0's words are one byte repeated */
.macro LOOP1 m
1:
    ldr r4, [r0, #0]
    ldr r5, [r0, #8]
    movs r3, r0
    adds r0, #16
    cmp r4, r5
    bne 4f
  .if \m
    ldr r5, [r0, #0]
    cmp r4, r5
    bne 4f
  .endif
    lsls r5, r4, #8
    lsrs r7, r4, #24
    orrs r5, r5, r7
    cmp r4, r5
    bne 4f
    FLAT
    b 9f
4:
    BOX \m, 0
9:
    adds r1, #10
    cmp r0, r9
    beq 8f
    b 1b
8:
.endm

/* two lines: flat when equal on both lines and one byte repeated, the
 * line alone when the lines are equal, else both averaged */
.macro LOOP2 m
1:
    ldmia r0!, {r4, r5, r6, r7} /* line 0, line 1, line 0, line 1 */
    cmp r4, r5
    bne 5f
    cmp r6, r7
    bne 5f
  .if \m
    ldr r5, [r0, #0]
    ldr r7, [r0, #4]
    cmp r5, r7
    bne 5f
    cmp r4, r5
    bne 4f
  .endif
    cmp r4, r6
    bne 4f
    lsls r5, r4, #8
    lsrs r7, r4, #24
    orrs r5, r5, r7
    cmp r4, r5
    bne 4f
    FLAT
    b 9f
4:
    movs r3, r0
    subs r3, #16
    BOX \m, 0
    b 9f
5:
    movs r3, r0
    subs r3, #16
    LOOK r4, r6, r3, OFF(\m, 0, 0), OFF(\m, 1, 0), r2
    LOOK r5, r6, r3, OFF(\m, 0, 1), OFF(\m, 1, 1), r2
    AVGST r4, r5, r6, r7, 0
    LOOK r4, r6, r3, OFF(\m, 2, 0), OFF(\m, 1, 0), r2
    LOOK r5, r6, r3, OFF(\m, 2, 1), OFF(\m, 1, 1), r2
    AVGST r4, r5, r6, r7, 2
    mov r7, r8
    LOOK r4, r6, r3, OFF(\m, 3, 0), OFF(\m, 4, 0), r7
    LOOK r5, r6, r3, OFF(\m, 3, 1), OFF(\m, 4, 1), r7
    AVGST r4, r5, r6, r7, 4
    LOOK r4, r6, r3, OFF(\m, 5, 0), OFF(\m, 6, 0), r2
    LOOK r5, r6, r3, OFF(\m, 5, 1), OFF(\m, 6, 1), r2
    AVGST r4, r5, r6, r7, 6
    LOOK r4, r6, r3, OFF(\m, 7, 0), OFF(\m, 6, 0), r2
    LOOK r5, r6, r3, OFF(\m, 7, 1), OFF(\m, 6, 1), r2
    AVGST r4, r5, r6, r7, 8
9:
    adds r1, #10
    cmp r0, r9
    beq 8f
    b 1b
8:
.endm

/* the loops' frame: saves r4-r10, sets r8-r10 */
.macro ENTER
    push {r4, r5, r6, r7, lr}
    mov r4, r8
    mov r5, r9
    mov r6, r10
    push {r4, r5, r6}
    movs r4, #1
    lsls r4, r4, #11
    adds r4, r4, r2
    mov r8, r4 /* pair44 */
    movs r4, #1
    lsls r4, r4, #9
    adds r4, r4, r0
    mov r9, r4 /* 32 groups of 16 bytes */
    movs r4, #0xf7
    lsls r4, r4, #8
    adds r4, #0xde
    mov r10, r4
.endm

.macro LEAVE
    pop {r4, r5, r6}
    mov r8, r4
    mov r9, r5
    mov r10, r6
    pop {r4, r5, r6, r7, pc}
.endm

/* one copy per misalignment, reached by long branches */
.macro DISPATCH name
    cmp r3, #0
    bne 1f
    b \name\()_m0
1:
    cmp r3, #1
    bne 1f
    b \name\()_m1
1:
    cmp r3, #2
    bne 1f
    b \name\()_m2
1:
    b \name\()_m3
.endm

    .section .text.nes_ppu_out1_asm, "ax", %progbits
    .global nes_ppu_out1_asm
    .type nes_ppu_out1_asm, %function
    .thumb_func
nes_ppu_out1_asm:
    ENTER
    DISPATCH .Lo1
.Lo1_m0:
    LOOP1 0
    LEAVE
.Lo1_m1:
    LOOP1 1
    LEAVE
.Lo1_m2:
    LOOP1 2
    LEAVE
.Lo1_m3:
    LOOP1 3
    LEAVE
    .size nes_ppu_out1_asm, . - nes_ppu_out1_asm

    .section .text.nes_ppu_out2_asm, "ax", %progbits
    .global nes_ppu_out2_asm
    .type nes_ppu_out2_asm, %function
    .thumb_func
nes_ppu_out2_asm:
    ENTER
    DISPATCH .Lo2
.Lo2_m0:
    LOOP2 0
    LEAVE
.Lo2_m1:
    LOOP2 1
    LEAVE
.Lo2_m2:
    LOOP2 2
    LEAVE
.Lo2_m3:
    LOOP2 3
    LEAVE
    .size nes_ppu_out2_asm, . - nes_ppu_out2_asm

/* the 33 tiles: expand[lo nibble | hi nibble << 4] | palette, the high
 * nibbles first */
    .section .text.nes_ppu_tiles_asm, "ax", %progbits
    .global nes_ppu_tiles_asm
    .type nes_ppu_tiles_asm, %function
    .thumb_func
nes_ppu_tiles_asm:
    push {r4, r5, r6, r7, lr}
    mov r4, r8
    mov r5, r9
    push {r4, r5}
    mov r8, r3 /* fine y */
    movs r4, #33
    lsls r4, r4, #3
    adds r4, r4, r0
    mov r9, r4 /* the tiles' end */
1:
    ldmia r0!, {r4, r5} /* pattern, palette word */
    add r4, r8
    ldrb r6, [r4, #8]
    ldrb r4, [r4, #0]
    movs r3, r4
    orrs r3, r6
    bne 2f
    str r5, [r1, #0] /* a blank row of the tile: its palette only */
    str r5, [r1, #8]
    adds r1, #16
    cmp r0, r9
    bne 1b
    b 3f
2:
    lsrs r3, r6, #4
    lsls r3, r3, #6
    lsrs r7, r4, #4
    lsls r7, r7, #2
    adds r3, r3, r7
    ldr r3, [r2, r3]
    orrs r3, r5
    lsls r6, r6, #28
    lsrs r6, r6, #22
    lsls r4, r4, #28
    lsrs r4, r4, #26
    adds r4, r4, r6
    ldr r4, [r2, r4]
    orrs r4, r5
    str r3, [r1, #0]
    str r4, [r1, #8]
    adds r1, #16
    cmp r0, r9
    bne 1b
3:
    pop {r4, r5}
    mov r8, r4
    mov r9, r5
    pop {r4, r5, r6, r7, pc}
    .size nes_ppu_tiles_asm, . - nes_ppu_tiles_asm

#endif
