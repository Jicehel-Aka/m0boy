/*
 * Hand-written Thumb-1 (Cortex-M0+) inner loops of the gbl scanline renderer.
 * Enabled by GBL_PPU_ASM (see gbl_options.h); gbl_ppu.c keeps the equivalent
 * C loops for other targets and as the reference.  With GBL_PPU_RAMFUNC the
 * loops run from SRAM (copied with .data at startup), which has no flash
 * wait state.
 *
 * Cycle counts are for the M0+ (loads/stores 2, LDM/STM 1+N, taken branch 2).
 */
#include "gbl_config.h"
#include "gbl_options.h"

#if GBL_PPU_ASM && defined(__ARM_ARCH_6M__)

    .syntax unified
    .thumb

#if GBL_PPU_RAMFUNC
#define GBL_SECTION(name) .section .data.gbl_ramfunc.name, "ax", %progbits
#else
#define GBL_SECTION(name) .section .text.name, "ax", %progbits
#endif

/*
 * void gbl_ppu_fetch_asm(uint32_t *t, const uint8_t *map, uint32_t n,
 *                        const uint8_t *trow, uint32_t idx_xor)
 *
 * For n map entries: t[i] = spread[lo] | spread[hi] << 1 where lo, hi are
 * the two bytes of row trow of tile map[i] ^ idx_xor (16 bytes per tile).
 * 16 instructions, about 22 cycles per tile (C: about 28).
 */
    GBL_SECTION(gbl_ppu_fetch_asm)
    .align 2
    .global gbl_ppu_fetch_asm
    .type gbl_ppu_fetch_asm, %function
    .thumb_func
gbl_ppu_fetch_asm:
    push    {r4-r7, lr}
    ldr     r4, [sp, #20]          /* idx_xor */
    ldr     r7, 3f                 /* spread table */
1:
    ldrb    r5, [r1]
    adds    r1, #1
    eors    r5, r4
    lsls    r5, r5, #4
    ldrh    r5, [r3, r5]           /* lo | hi << 8 (tile rows are 2-aligned) */
    lsls    r6, r5, #24
    lsrs    r6, r6, #23            /* lo * 2 */
    lsrs    r5, r5, #8
    lsls    r5, r5, #1             /* hi * 2 */
    ldrh    r6, [r7, r6]
    ldrh    r5, [r7, r5]
    lsls    r5, r5, #1
    orrs    r5, r6
    stm     r0!, {r5}
    subs    r2, #1
    bne     1b
    pop     {r4-r7, pc}
    .align 2
3:  .word   gbl_ppu_spread
    .size gbl_ppu_fetch_asm, . - gbl_ppu_fetch_asm

/*
 * void gbl_ppu_convert_asm(uint32_t *out, const uint16_t *bg,
 *                          const uint32_t *pairs, uint32_t groups)
 * void gbl_ppu_convert4_asm(...)  same, groups read every 4 bytes (the low
 *                          halves of the fetched tile words, when there is
 *                          no fine scroll to extract).
 *
 * Each 16-bit group of eight 2-bit pixels (leftmost in bits 15:14) becomes
 * four words pairs[nibble] (two RGB565 pixels each), stored with one STM.
 * The nibble is extracted already scaled by 4 (shift, then AND 0x3C), which
 * GCC does not do; the four colours stay in r4-r7 for the STM.
 * 17 instructions, about 27 cycles per 8 pixels (C: about 33).
 */
.macro GBL_CONVERT name, stride
    GBL_SECTION(\name)
    .align 2
    .global \name
    .type \name, %function
    .thumb_func
\name:
    push    {r4-r7}
    lsls    r3, r3, #(\stride / 2)
    adds    r3, r1
    mov     ip, r3                 /* end of input */
    movs    r3, #0x3C
1:
    ldrh    r4, [r1]
    adds    r1, #\stride
    lsrs    r5, r4, #6
    ands    r5, r3
    ldr     r5, [r2, r5]           /* pixels 2-3 */
    lsrs    r6, r4, #2
    ands    r6, r3
    ldr     r6, [r2, r6]           /* pixels 4-5 */
    lsls    r7, r4, #2
    ands    r7, r3
    ldr     r7, [r2, r7]           /* pixels 6-7 */
    lsrs    r4, r4, #10
    ands    r4, r3
    ldr     r4, [r2, r4]           /* pixels 0-1 */
    stm     r0!, {r4-r7}
    cmp     r1, ip
    bne     1b
    pop     {r4-r7}
    bx      lr
    .size \name, . - \name
.endm

    GBL_CONVERT gbl_ppu_convert_asm, 2
    GBL_CONVERT gbl_ppu_convert4_asm, 4

/*
 * void gbl_ppu_convert_fine_asm(uint32_t *out, const uint32_t *t,
 *                               const uint32_t *pairs, uint32_t shift)
 *
 * Same conversion straight from 21 fetched tile words with fine scroll:
 * group g is (t[g] << 16 | t[g + 1]) >> shift (shift = 16 - 2 * fine).
 * Bits above 15 are left in place: every index is masked with 0x3C anyway.
 * This replaces the C extraction loop and its copy (about 13 cycles per
 * group) for 5 more cycles here: about 32 cycles per 8 pixels.
 */
    GBL_SECTION(gbl_ppu_convert_fine_asm)
    .align 2
    .global gbl_ppu_convert_fine_asm
    .type gbl_ppu_convert_fine_asm, %function
    .thumb_func
gbl_ppu_convert_fine_asm:
    push    {r4-r7}
    mov     r4, r8
    push    {r4}
    mov     r8, r3                 /* shift */
    movs    r4, #80                /* 20 groups, one tile word each */
    adds    r4, r1
    mov     ip, r4
    movs    r3, #0x3C
1:
    ldm     r1!, {r4}              /* t[g] */
    ldr     r5, [r1]               /* t[g + 1] */
    lsls    r4, r4, #16
    orrs    r4, r5
    mov     r6, r8
    lsrs    r4, r6
    lsrs    r5, r4, #6
    ands    r5, r3
    ldr     r5, [r2, r5]           /* pixels 2-3 */
    lsrs    r6, r4, #2
    ands    r6, r3
    ldr     r6, [r2, r6]           /* pixels 4-5 */
    lsls    r7, r4, #2
    ands    r7, r3
    ldr     r7, [r2, r7]           /* pixels 6-7 */
    lsrs    r4, r4, #10
    ands    r4, r3
    ldr     r4, [r2, r4]           /* pixels 0-1 */
    stm     r0!, {r4-r7}
    cmp     r1, ip
    bne     1b
    pop     {r4}
    mov     r8, r4
    pop     {r4-r7}
    bx      lr
    .size gbl_ppu_convert_fine_asm, . - gbl_ppu_convert_fine_asm

#endif
