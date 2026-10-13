/* SG-1000 device renderer, Thumb-1 (Cortex-M0+): the tile loops of
 * tms9918.c (out_line, out_pair) for Graphics I and II, same results.
 *
 * Every argument lives in a register for the whole loop (the C version
 * reloads VDP bytes after each store, which may alias them).  A tile costs
 * its 3 or 5 VRAM bytes, one weight word and 5 blend reads and writes; the
 * weight fields come out with two shifts.  The loops run from SRAM: no
 * flash wait states.
 *
 * Arguments (sg_tile_args, tms9918.c):
 *   0 out   4 names   8 pt   12 ct   16 mix   20 third   24 pmask   28 cmask
 * mix: uint16_t [256][9], row c at mix + 18c, entry w at +2w.
 * sg_tile_w: five 5-bit fields of 2w; sg_tile_wv: the same fields of w. */
#include "sg_config.h"

#if defined(__ARM_ARCH_6M__) && !SG_LOW_RAM && !defined(SG_NO_RENDER_ASM)

    .syntax unified
    .thumb

#define SG_RAMFUNC(name) .section .data.sg_ramfunc.name, "ax", %progbits

/* one device pixel: field at bits [shift_up .. ] of w, blend row in row */
.macro OUT2W w, row, tmp, up, slot
    lsls \tmp, \w, #\up
    lsrs \tmp, \tmp, #27
    ldrh \tmp, [\row, \tmp]
    strh \tmp, [r0, #\slot]
.endm
/* the same from a sum of two weight words plus 1 per field: offset =
 * (field & 30), i.e. 2 * ((wa + wb + 1) / 2) */
.macro OUTMEAN w, row, tmp, up, slot
    lsls \tmp, \w, #\up
    lsrs \tmp, \tmp, #28
    lsls \tmp, \tmp, #1
    ldrh \tmp, [\row, \tmp]
    strh \tmp, [r0, #\slot]
.endm

.macro ENTER
    push {r4-r7, lr}
    mov r4, r8
    mov r5, r9
    mov r6, r10
    mov r7, r11
    push {r4-r7}
.endm
.macro LEAVE
    pop {r4-r7}
    mov r8, r4
    mov r9, r5
    mov r10, r6
    mov r11, r7
    pop {r4-r7, pc}
.endm
/* r0 = args -> r0 out, r1 names, r2 pt, r3 ct, r8 mix, lr end of names */
.macro LOAD_ARGS
    ldr r1, [r0, #4]
    ldr r2, [r0, #8]
    ldr r3, [r0, #12]
    ldr r4, [r0, #16]
    mov r8, r4
    ldr r4, [r0, #20]
    mov r9, r4
    ldr r4, [r0, #24]
    mov r10, r4
    ldr r4, [r0, #28]
    mov r11, r4
    movs r4, #32
    adds r4, r4, r1
    mov lr, r4
    ldr r0, [r0, #0]
.endm

/* ---- one line ----------------------------------------------------------- */

    SG_RAMFUNC(sg_tiles_g1)
    .align 2
    .global sg_tiles_g1
    .type sg_tiles_g1, %function
    .thumb_func
/* void sg_tiles_g1(const sg_tile_args *) -- pt already points at the row */
sg_tiles_g1:
    ENTER
    LOAD_ARGS
    ldr r7, =sg_tile_w
1:  ldrb r4, [r1]
    adds r1, r1, #1
    lsls r5, r4, #3
    ldrb r5, [r2, r5]          /* pattern */
    lsrs r4, r4, #3
    ldrb r4, [r3, r4]          /* colour byte */
    lsls r5, r5, #2
    ldr r5, [r7, r5]           /* weights */
    movs r6, #18
    muls r4, r6, r4
    add r4, r8                 /* blend row */
    OUT2W r5, r4, r6, 27, 0
    OUT2W r5, r4, r6, 22, 2
    OUT2W r5, r4, r6, 17, 4
    OUT2W r5, r4, r6, 12, 6
    lsrs r6, r5, #20
    ldrh r6, [r4, r6]
    strh r6, [r0, #8]
    adds r0, r0, #10
    cmp r1, lr
    bne 1b
    LEAVE
    .pool
    .size sg_tiles_g1, . - sg_tiles_g1

    SG_RAMFUNC(sg_tiles_g2)
    .align 2
    .global sg_tiles_g2
    .type sg_tiles_g2, %function
    .thumb_func
/* void sg_tiles_g2(const sg_tile_args *): offset = tile * 8 | third,
 * pattern at pt[offset & pmask], colours at ct[offset & cmask] */
sg_tiles_g2:
    ENTER
    LOAD_ARGS
    ldr r7, =sg_tile_w
1:  ldrb r4, [r1]
    adds r1, r1, #1
    lsls r4, r4, #3
    mov r5, r9
    orrs r4, r5                /* offset */
    mov r5, r10
    ands r5, r4
    ldrb r5, [r2, r5]          /* pattern */
    mov r6, r11
    ands r4, r6
    ldrb r4, [r3, r4]          /* colour byte */
    lsls r5, r5, #2
    ldr r5, [r7, r5]
    movs r6, #18
    muls r4, r6, r4
    add r4, r8
    OUT2W r5, r4, r6, 27, 0
    OUT2W r5, r4, r6, 22, 2
    OUT2W r5, r4, r6, 17, 4
    OUT2W r5, r4, r6, 12, 6
    lsrs r6, r5, #20
    ldrh r6, [r4, r6]
    strh r6, [r0, #8]
    adds r0, r0, #10
    cmp r1, lr
    bne 1b
    LEAVE
    .pool
    .size sg_tiles_g2, . - sg_tiles_g2

/* ---- two lines (ya, ya + 1) of one tile row ----------------------------- */

    SG_RAMFUNC(sg_pair_g1)
    .align 2
    .global sg_pair_g1
    .type sg_pair_g1, %function
    .thumb_func
/* void sg_pair_g1(const sg_tile_args *): one colour byte per tile */
sg_pair_g1:
    ENTER
    LOAD_ARGS
    ldr r7, =sg_tile_wv
    ldr r4, =0x0108421
    mov r9, r4                 /* +1 per field */
1:  ldrb r4, [r1]
    adds r1, r1, #1
    lsls r5, r4, #3
    adds r5, r5, r2
    ldrb r6, [r5, #0]          /* pattern, line a */
    ldrb r5, [r5, #1]          /* pattern, line b */
    lsrs r4, r4, #3
    ldrb r4, [r3, r4]          /* colour byte */
    lsls r6, r6, #2
    ldr r6, [r7, r6]
    lsls r5, r5, #2
    ldr r5, [r7, r5]
    adds r5, r5, r6
    add r5, r9                 /* w_a + w_b + 1 per field */
    movs r6, #18
    muls r4, r6, r4
    add r4, r8
    OUTMEAN r5, r4, r6, 27, 0
    OUTMEAN r5, r4, r6, 22, 2
    OUTMEAN r5, r4, r6, 17, 4
    OUTMEAN r5, r4, r6, 12, 6
    OUTMEAN r5, r4, r6, 7, 8
    adds r0, r0, #10
    cmp r1, lr
    bne 1b
    LEAVE
    .pool
    .size sg_pair_g1, . - sg_pair_g1

/* two colour bytes: the two lines' blends averaged per channel (avg565 in
 * tms9918.c), rows in r5/r4, weights in r6/r7, mask 0xf7de in r12 */
.macro AVG up, slot
    lsls r1, r6, #\up
    lsrs r1, r1, #27
    ldrh r1, [r5, r1]
    lsls r2, r7, #\up
    lsrs r2, r2, #27
    ldrh r2, [r4, r2]
#if SG_PANEL_SWAP
    rev16 r1, r1
    rev16 r2, r2
#endif
    movs r3, r1
    eors r3, r2
    ands r1, r2
    mov r2, r12
    ands r3, r2
    lsrs r3, r3, #1
    adds r1, r1, r3
#if SG_PANEL_SWAP
    rev16 r1, r1
#endif
    strh r1, [r0, #\slot]
.endm

    SG_RAMFUNC(sg_pair_g2)
    .align 2
    .global sg_pair_g2
    .type sg_pair_g2, %function
    .thumb_func
/* void sg_pair_g2(const sg_tile_args *): the second line's bytes follow the
 * first's (pairs stay inside a tile row, so offset + 1 never wraps a mask) */
sg_pair_g2:
    ENTER
    LOAD_ARGS
    ldr r4, =0xf7de
    mov r12, r4
1:  ldrb r4, [r1]
    adds r1, r1, #1
    lsls r4, r4, #3
    mov r5, r9
    orrs r4, r5                /* offset (line a) */
    mov r5, r10
    ands r5, r4
    adds r5, r5, r2
    ldrb r6, [r5, #0]          /* pattern, line a */
    ldrb r7, [r5, #1]          /* pattern, line b */
    mov r5, r11
    ands r4, r5
    adds r4, r4, r3
    ldrb r5, [r4, #0]          /* colour byte, line a */
    ldrb r4, [r4, #1]          /* colour byte, line b */
    cmp r5, r4
    bne 3f
    ldr r4, =sg_tile_wv
    lsls r6, r6, #2
    ldr r6, [r4, r6]
    lsls r7, r7, #2
    ldr r7, [r4, r7]
    adds r6, r6, r7
    ldr r7, =0x0108421
    adds r6, r6, r7            /* w_a + w_b + 1 per field */
    movs r7, #18
    muls r5, r7, r5
    add r5, r8
    OUTMEAN r6, r5, r7, 27, 0
    OUTMEAN r6, r5, r7, 22, 2
    OUTMEAN r6, r5, r7, 17, 4
    OUTMEAN r6, r5, r7, 12, 6
    OUTMEAN r6, r5, r7, 7, 8
2:  adds r0, r0, #10
    cmp r1, lr
    bne 1b
    LEAVE
3:  push {r1-r3}
    movs r1, #18
    muls r5, r1, r5
    muls r4, r1, r4
    add r5, r8                 /* blend row, line a */
    add r4, r8                 /* blend row, line b */
    ldr r1, =sg_tile_w
    lsls r6, r6, #2
    ldr r6, [r1, r6]
    lsls r7, r7, #2
    ldr r7, [r1, r7]
    AVG 27, 0
    AVG 22, 2
    AVG 17, 4
    AVG 12, 6
    AVG 7, 8
    pop {r1-r3}
    b 2b
    .pool
    .size sg_pair_g2, . - sg_pair_g2

#endif
