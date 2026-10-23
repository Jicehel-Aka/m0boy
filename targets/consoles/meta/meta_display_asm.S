/*
 * Thumb-1 (Cortex-M0+) SMOOTH row blend of meta_display.c (smooth_blend;
 * the C loop there is the reference and runs on the host).
 *
 * void meta_smooth_blend_asm(uint32_t *row, const uint16_t *upper,
 *                            const uint16_t *lower, const uint32_t *pairs,
 *                            uint32_t unused)
 *
 * upper, lower: packed lines, twenty 16-bit groups of eight 2-bit shades
 *               (left pixel in bits 15:14).
 * pairs:        256 entries, index upper nibble | lower nibble << 4, the
 *               two colours of that pixel pair (left one in the low half).
 * row:          receives 80 words (160 colours).
 *
 * Per group: y = (u & 0x0F0F) | (l & 0x0F0F) << 4 and
 * z = (u >> 4 & 0x0F0F) | (l & 0xF0F0) interleave the nibbles, so that the
 * four table indices are bytes: z >> 8 (pixels 0-1), y >> 8 (2-3), z (4-5),
 * y (6-7).  About 38 cycles per group of eight pixels.
 */
#if defined(__ARM_ARCH_6M__) && !(defined(META_SMOOTH_ASM) && META_SMOOTH_ASM == 0)

    .syntax unified
    .thumb

/* r7 = byte index * 4 from \src >> \shift, then one stored entry */
.macro PAIR src, shift
    .if \shift
    lsrs    r7, \src, #\shift
    lsls    r7, r7, #24
    .else
    lsls    r7, \src, #24
    .endif
    lsrs    r7, r7, #22
    ldr     r7, [r3, r7]
    stm     r0!, {r7}
.endm

    .section .text.meta_smooth_blend_asm, "ax", %progbits
    .align 2
    .global meta_smooth_blend_asm
    .type meta_smooth_blend_asm, %function
    .thumb_func
meta_smooth_blend_asm:
    push    {r4-r7}
    mov     r4, r8
    push    {r4}
    ldr     r4, 9f
    mov     r8, r4                 /* 0x0F0F */
    movs    r4, #40
    adds    r4, r4, r1
    mov     ip, r4                 /* end of the upper line */
1:
    ldrh    r4, [r1]               /* u */
    ldrh    r5, [r2]               /* l */
    adds    r1, #2
    adds    r2, #2
    lsrs    r6, r4, #4
    mov     r7, r8
    ands    r6, r7                 /* u >> 4 & 0x0F0F */
    ands    r4, r7                 /* u & 0x0F0F */
    lsls    r7, r7, #4             /* 0xF0F0 */
    ands    r7, r5
    orrs    r6, r7                 /* z */
    mov     r7, r8
    ands    r5, r7
    lsls    r5, r5, #4
    orrs    r4, r5                 /* y */
    PAIR    r6, 8
    PAIR    r4, 8
    PAIR    r6, 0
    PAIR    r4, 0
    cmp     r1, ip
    bne     1b
    pop     {r4}
    mov     r8, r4
    pop     {r4-r7}
    bx      lr
    .align 2
9:  .word   0x0F0F
    .size meta_smooth_blend_asm, . - meta_smooth_blend_asm

#endif
