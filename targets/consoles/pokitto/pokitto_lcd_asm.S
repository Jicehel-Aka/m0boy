/*
 * Pokitto pixel pushes in Thumb-1, run from SRAM (copied with .data, no
 * flash wait states), after FManga's FLUSH_QUAD_SCALE in gm0:
 *
 *   r1 = &MPIN[2]  data lines (MASK[2] leaves only P2_3..P2_18 writable)
 *   r1 + r5        CLR[1]   (r5 = 0xFC)  WR low
 *   r1 + 0x7C      SET[1]                WR high: the panel latches
 *   r2 = 1 << 12   WR (P1_12)
 *
 * Each pixel is ldrh, lsls #3 and three stores (about 9 cycles); the load of
 * the next pixel sits between WR low and WR high, so the strobe lasts a few
 * cycles (gm0 keeps one instruction there, PokittoLib a nop).  The 200x176
 * mode latches the second pixel of every four twice (the bus unchanged,
 * only WR toggled), 160 -> 200.  pokitto_lcd.c keeps the C versions
 * (POKITTO_LCD_ASM=0) as the reference.
 */
#include "gbl_profile.h"
#if POKITTO_LCD_ASM
    .syntax unified
    .cpu cortex-m0plus
    .thumb

#define MPIN2 0xA0002188

/* Latch pixel register \cur (already shifted) and load + shift \next from
 * [r0, #\off] while WR is low. */
.macro PIX cur, next, off
    str \cur, [r1]
    str r2, [r1, r5]
    ldrh \next, [r0, #\off]
    str r2, [r1, #0x7C]
    lsls \next, \next, #3
.endm

/* Last pixel of an iteration: nothing to load, lsls keeps the strobe wide. */
.macro PIX_LAST cur
    str \cur, [r1]
    str r2, [r1, r5]
    adds r0, #16
    str r2, [r1, #0x7C]
.endm

/* WR strobe alone: the pixel on the bus once more. */
.macro DUP
    str r2, [r1, r5]
    nop
    str r2, [r1, #0x7C]
.endm

/* One bus write of the C reference (command(), data(), strobe()): the value
 * in \val goes on the data lines, WR low for two nops. */
.macro STROBE
    str r6, [r2, r5]
    nop
    nop
    str r6, [r2, #0x7C]
.endm

    .section .data.gbl_ramfunc.pokitto_at, "ax", %progbits
    .align 2
    .global pokitto_at
    .type pokitto_at, %function
    .thumb_func
/* void pokitto_at(unsigned x, unsigned y): GRAM write from (x, y) on,
 * at() of pokitto_lcd.c: registers 0x20 (y) and 0x21 (x), command 0x22,
 * with the same writes and strobes, from SRAM (no flash wait states).
 *   r2 = &MPIN[2]  r3 = CD (P0_2)  r4 = 0xF8 (CLR[0])  r5 = 0xFC (CLR[1])
 *   r6 = WR (P1_12)  SET[0] = r2 + 0x78, SET[1] = r2 + 0x7C */
pokitto_at:
    push {r4, r5, r6, r7, lr}
    ldr r2, =MPIN2
    movs r3, #4
    movs r4, #0xF8
    movs r5, #0xFC
    movs r6, #1
    lsls r6, r6, #12
    lsls r0, r0, #3
    lsls r1, r1, #3
    movs r7, #1
    lsls r7, r7, #8
    str r3, [r2, r4]           /* command 0x20: CD low */
    str r7, [r2]
    STROBE
    str r3, [r2, #0x78]        /* CD high */
    str r1, [r2]               /* data y */
    STROBE
    adds r7, #8
    str r3, [r2, r4]           /* command 0x21 */
    str r7, [r2]
    STROBE
    str r3, [r2, #0x78]
    str r0, [r2]               /* data x */
    STROBE
    adds r7, #8
    str r3, [r2, r4]           /* command 0x22 */
    str r7, [r2]
    STROBE
    str r3, [r2, #0x78]
    pop {r4, r5, r6, r7, pc}
    .pool
    .size pokitto_at, . - pokitto_at

    .section .data.gbl_ramfunc.pokitto_push160, "ax", %progbits
    .align 2
    .global pokitto_push160
    .type pokitto_push160, %function
    .thumb_func
/* void pokitto_push160(const uint16_t *px): 160 pixels at the GRAM cursor. */
pokitto_push160:
    push {r4, r5, r6, r7, lr}
    ldr r1, =MPIN2
    movs r2, #1
    lsls r2, r2, #12
    movs r5, #0xFC
    movs r3, r0
    adds r3, #255
    adds r3, #65            /* r3 = px + 320 bytes */
    ldrh r6, [r0]
    lsls r6, r6, #3
1:  PIX r6, r7, 2
    PIX r7, r6, 4
    PIX r6, r7, 6
    PIX r7, r6, 8
    PIX r6, r7, 10
    PIX r7, r6, 12
    PIX r6, r7, 14
    PIX_LAST r7
    cmp r0, r3
    beq 2f
    ldrh r6, [r0]
    lsls r6, r6, #3
    b 1b
2:  pop {r4, r5, r6, r7, pc}
    .pool
    .size pokitto_push160, . - pokitto_push160

    .section .data.gbl_ramfunc.pokitto_push200, "ax", %progbits
    .align 2
    .global pokitto_push200
    .type pokitto_push200, %function
    .thumb_func
/* void pokitto_push200(const uint16_t *px): 160 pixels stretched to 200
 * (a b b c d per four) at the GRAM cursor. */
pokitto_push200:
    push {r4, r5, r6, r7, lr}
    ldr r1, =MPIN2
    movs r2, #1
    lsls r2, r2, #12
    movs r5, #0xFC
    movs r3, r0
    adds r3, #255
    adds r3, #65
    ldrh r6, [r0]
    lsls r6, r6, #3
1:  PIX r6, r7, 2           /* a */
    PIX r7, r6, 4           /* b */
    DUP                     /* b again */
    PIX r6, r7, 6           /* c */
    PIX r7, r6, 8           /* d */
    PIX r6, r7, 10          /* a */
    PIX r7, r6, 12          /* b */
    DUP
    PIX r6, r7, 14          /* c */
    PIX_LAST r7             /* d */
    cmp r0, r3
    beq 2f
    ldrh r6, [r0]
    lsls r6, r6, #3
    b 1b
2:  pop {r4, r5, r6, r7, pc}
    .pool
    .size pokitto_push200, . - pokitto_push200
#endif
