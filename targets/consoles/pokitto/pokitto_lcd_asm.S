/*
 * Pokitto pixel pushes in Thumb-1, run from SRAM (copied with .data, no
 * flash wait states), after FManga's FLUSH_QUAD_SCALE in gm0:
 *
 *   r1 = &MPIN[2]  data lines (MASK[2] leaves only P2_3..P2_18 writable)
 *   r1 + r5        CLR[1]   (r5 = 0xFC)  WR low
 *   r1 + 0x7C      SET[1]                WR high: the panel latches
 *   r2 = 1 << 12   WR (P1_12)
 *
 * Each pixel is three stores and its shifts (about 9 cycles), the next
 * pixel's shift between WR low and WR high (gm0 keeps one instruction
 * there, PokittoLib a nop); four pixels already on the bus are four or
 * five strobes alone.  The 200x176
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

/* WR strobe alone: the pixel on the bus once more. */
.macro DUP
    str r2, [r1, r5]
    nop
    str r2, [r1, #0x7C]
.endm

/* Four pixels at a time from r0 up to r3, as two words: when both hold the
 * colour already on the bus twice (r4: the last pixel written, in both
 * halves) they are strobes alone, the bus unchanged; otherwise each
 * strobe's gap does the next pixel's shifts.  stretch: the second pixel of
 * the four latched twice (160 -> 200).  Ends at 2f. */
.macro PUSH4 stretch
    ldr r4, [r0]
    mvns r4, r4             /* never the first word: the first group is written */
1:  ldr r6, [r0, #0]
    cmp r6, r4
    bne 3f
    ldr r7, [r0, #4]
    cmp r7, r4
    bne 4f
    DUP
    DUP
    DUP
    DUP
  .if \stretch
    DUP
  .endif
    adds r0, #8
    cmp r0, r3
    bne 1b
    b 2f
3:  ldr r7, [r0, #4]
4:  lsls r4, r6, #16
    lsrs r4, r4, #13        /* a << 3 */
    str r4, [r1]            /* a */
    str r2, [r1, r5]
    lsrs r6, r6, #16
    str r2, [r1, #0x7C]
    lsls r6, r6, #3
    str r6, [r1]            /* b */
    str r2, [r1, r5]
    lsls r4, r7, #16
    str r2, [r1, #0x7C]
    lsrs r4, r4, #13
  .if \stretch
    str r2, [r1, r5]        /* b again */
    lsrs r6, r7, #16
    str r2, [r1, #0x7C]
    lsls r6, r6, #3
    str r4, [r1]            /* c */
    str r2, [r1, r5]
    lsrs r4, r7, #16
    str r2, [r1, #0x7C]
  .else
    str r4, [r1]            /* c */
    str r2, [r1, r5]
    lsrs r6, r7, #16
    str r2, [r1, #0x7C]
    lsls r6, r6, #3
    lsrs r4, r7, #16
  .endif
    str r6, [r1]            /* d */
    str r2, [r1, r5]
    lsls r7, r4, #16
    str r2, [r1, #0x7C]
    orrs r4, r7             /* d on the bus, in both halves */
    adds r0, #8
    cmp r0, r3
    bne 1b
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

    .section .data.gbl_ramfunc.pokitto_push_n, "ax", %progbits
    .align 2
    .global pokitto_push160
    .type pokitto_push160, %function
    .thumb_func
/* void pokitto_push160(const uint16_t *px): 160 pixels at the GRAM cursor. */
pokitto_push160:
    movs r1, #160
    .global pokitto_push_n
    .type pokitto_push_n, %function
    .thumb_func
/* void pokitto_push_n(const uint16_t *px, unsigned n): n pixels (a multiple
 * of four, px 4-aligned) at the GRAM cursor, as pokitto_push200 without its
 * stretch: four pixels as two words, five strobes alone when both hold the
 * colour already on the bus (r4), else each strobe's gap doing the next
 * pixel's shifts.  The Game Boy's 1:1 rows (160) and the PICO-8's (128,
 * pokitto_p8lcd.c). */
pokitto_push_n:
    push {r4, r5, r6, r7, lr}
    lsls r3, r1, #1
    adds r3, r3, r0         /* r3 = px + 2n bytes */
    ldr r1, =MPIN2
    movs r2, #1
    lsls r2, r2, #12
    movs r5, #0xFC
    PUSH4 0
2:  pop {r4, r5, r6, r7, pc}
    .pool
    .size pokitto_push_n, . - pokitto_push_n

    .section .data.gbl_ramfunc.pokitto_push200, "ax", %progbits
    .align 2
    .global pokitto_push200
    .type pokitto_push200, %function
    .thumb_func
/* void pokitto_push200(const uint16_t *px): 160 pixels stretched to 200
 * (a b b c d per four) at the GRAM cursor, PUSH4's stretch: a run of sky
 * is five strobes per four pixels.  px is 4-aligned. */
pokitto_push200:
    push {r4, r5, r6, r7, lr}
    ldr r1, =MPIN2
    movs r2, #1
    lsls r2, r2, #12
    movs r5, #0xFC
    movs r3, r0
    adds r3, #255
    adds r3, #65            /* r3 = px + 320 bytes */
    PUSH4 1
2:  pop {r4, r5, r6, r7, pc}
    .pool
    .size pokitto_push200, . - pokitto_push200
#endif
