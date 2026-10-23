/*
 * Thumb-1 (Cortex-M0+) RGB444 row packing of meta_lcd_pack.h (the C
 * function there is the reference and runs on the host).
 *
 * void meta_lcd_pack444_asm(uint8_t *p, unsigned pixels)
 *
 * p: 4-byte aligned, `pixels` (a multiple of 8) 0x0RGB halfwords, packed in
 * place.  Per word w (pixel a low, b high): Q = (a << 12) | b, the 24-bit
 * big-endian pair; four words give twelve bytes, stored as three words:
 *   W0 = rev(Q0 << 8) | (Q1 >> 16) << 24
 *   W1 = rev(Q1 << 16 | Q2 >> 8)
 *   W2 = rev(Q2 << 24 | Q3)
 * About 40 cycles per eight pixels.
 */
#if defined(__ARM_ARCH_6M__)

    .syntax unified
    .thumb

/* \w = (\w << 20 >> 8) | (\w >> 16) */
.macro PAIRQ w
    lsls    r6, \w, #20
    lsrs    r6, r6, #8
    lsrs    \w, \w, #16
    orrs    \w, r6
.endm

    .section .text.meta_lcd_pack444_asm, "ax", %progbits
    .align 2
    .global meta_lcd_pack444_asm
    .type meta_lcd_pack444_asm, %function
    .thumb_func
meta_lcd_pack444_asm:
    push    {r4-r7}
    lsls    r1, r1, #1
    adds    r1, r1, r0             /* end of the source */
    movs    r7, r0                 /* source; r0 = destination */
    mov     ip, r1
1:
    ldm     r7!, {r2-r5}
    PAIRQ   r2
    PAIRQ   r3
    PAIRQ   r4
    PAIRQ   r5
    lsls    r2, r2, #8
    rev     r2, r2
    lsrs    r6, r3, #16
    lsls    r6, r6, #24
    orrs    r2, r6                 /* W0 */
    lsls    r3, r3, #16
    lsrs    r6, r4, #8
    orrs    r3, r6
    rev     r3, r3                 /* W1 */
    lsls    r4, r4, #24
    orrs    r4, r5
    rev     r4, r4                 /* W2 */
    stm     r0!, {r2-r4}
    cmp     r7, ip
    bne     1b
    pop     {r4-r7}
    bx      lr
    .size meta_lcd_pack444_asm, . - meta_lcd_pack444_asm

#endif
