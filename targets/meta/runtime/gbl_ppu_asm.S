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

/*
 * uint32_t gbl_ppu_tiles_hash_asm(const uint8_t *map_row, uint32_t col_xor,
 *                                 uint32_t count, const uint16_t *ver)
 *
 * The line-signature loop of tiles_hash (gbl_ppu.c): for `count` entries
 * of the 32-entry map row from column col_xor & 0xFF (wrapping),
 * s = (s ^ ver[idx ^ (col_xor >> 8)]) * gbl_sig_mul; s ^= s >> 15, from
 * the FNV offset basis.  The wrap splits the row into two pointer runs, so
 * no index masking: 14 cycles per tile instead of 19, and a five-register
 * frame instead of nine.
 */
    GBL_SECTION(gbl_ppu_tiles_hash_asm)
    .align 2
    .global gbl_ppu_tiles_hash_asm
    .type gbl_ppu_tiles_hash_asm, %function
    .thumb_func
gbl_ppu_tiles_hash_asm:
    push    {r4-r7, lr}
    lsrs    r6, r1, #8             /* tile index xor */
    uxtb    r1, r1
    adds    r0, r0, r1             /* first entry */
    movs    r5, #32
    subs    r5, r5, r1             /* entries before the wrap */
    subs    r2, r2, r5             /* entries after it */
    bhi     1f
    adds    r5, r5, r2             /* no wrap: all count entries */
    movs    r2, #0
1:
    adds    r1, r0, r5             /* end of this run */
    ldr     r4, 8f
    ldr     r4, [r4]               /* gbl_sig_mul */
    ldr     r7, 9f                 /* s */
    b       3f
2:
    ldrb    r5, [r0]
    adds    r0, #1
    eors    r5, r6
    lsls    r5, r5, #1
    ldrh    r5, [r3, r5]
    eors    r7, r5
    muls    r7, r4, r7
    lsrs    r5, r7, #15
    eors    r7, r5
3:
    cmp     r0, r1
    bne     2b
    cmp     r2, #0
    beq     4f
    subs    r0, #32                /* wrap to column 0 */
    adds    r1, r0, r2
    movs    r2, #0
    b       2b
4:
    movs    r0, r7
    pop     {r4-r7, pc}
    .align 2
8:  .word   gbl_sig_mul
9:  .word   2166136261
    .size gbl_ppu_tiles_hash_asm, . - gbl_ppu_tiles_hash_asm

#endif
