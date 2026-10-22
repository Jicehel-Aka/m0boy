/* Fixed-size copies for the card cache and the ROM pool (m0_copy.h), Thumb-1:
 * m0_copy<n>_<k>(dst, src) copies n bytes (16, 32, 64, 128, 256 or 512) to
 * a word-aligned dst from a src k bytes past a word boundary (k = src & 3).
 *   k = 0  ldm/stm of two words, fully unrolled up to 64 bytes; four words
 *          twice per turn above (6 cycles per 8 bytes, 23 per 32)
 *   k > 0  aligned words joined by shifts: each output word is the end of
 *          one source word and the start of the next (7 cycles per word);
 *          the source is read up to the word holding its last byte, never
 *          past it
 * Unrolled up to 64 bytes, then 32 bytes per turn.  Sizes are always
 * known: no length, no alignment test, no tail. */
    .syntax unified
    .thumb
#ifndef M0_COPY_RAMFUNC
#define M0_COPY_RAMFUNC 0
#endif

.macro m0_copy_begin name
#if M0_COPY_RAMFUNC
    .section .data.\name, "ax", %progbits
#else
    .section .text.\name, "ax", %progbits
#endif
    .align 2
    .global \name
    .type \name, %function
    .thumb_func
\name:
.endm

/* aligned: 8 bytes a step through r2, r3 */
.macro m0_copy_pairs bytes
    .rept \bytes / 8
    ldm     r1!, {r2, r3}
    stm     r0!, {r2, r3}
    .endr
.endm

/* shifted by k: r2 holds the source word before, two output words a step
 * (the registers swap roles instead of a move) */
.macro m0_copy_shift_pair k
    ldm     r1!, {r3}
    lsrs    r2, r2, #(8 * \k)
    lsls    r4, r3, #(32 - 8 * \k)
    orrs    r2, r4
    stm     r0!, {r2}
    ldm     r1!, {r2}
    lsrs    r3, r3, #(8 * \k)
    lsls    r4, r2, #(32 - 8 * \k)
    orrs    r3, r4
    stm     r0!, {r3}
.endm

.macro m0_copy_aligned bytes
    m0_copy_begin m0_copy\bytes\()_0
.if \bytes <= 64
    m0_copy_pairs \bytes
    bx      lr
.else
    push    {r4-r7}
    movs    r2, #(\bytes / 32)
1:  ldm     r1!, {r4-r7}
    stm     r0!, {r4-r7}
    ldm     r1!, {r4-r7}
    stm     r0!, {r4-r7}
    subs    r2, #1
    bne     1b
    pop     {r4-r7}
    bx      lr
.endif
    .size m0_copy\bytes\()_0, . - m0_copy\bytes\()_0
.endm

.macro m0_copy_shifted bytes, k
    m0_copy_begin m0_copy\bytes\()_\k
.if \bytes <= 64
    push    {r4}
    subs    r1, #\k
    ldm     r1!, {r2}
    .rept \bytes / 8
    m0_copy_shift_pair \k
    .endr
    pop     {r4}
    bx      lr
.else
    push    {r4, r5}
    subs    r1, #\k
    ldm     r1!, {r2}
    movs    r5, #(\bytes / 32)
1:  m0_copy_shift_pair \k
    m0_copy_shift_pair \k
    m0_copy_shift_pair \k
    m0_copy_shift_pair \k
    subs    r5, #1
    bne     1b
    pop     {r4, r5}
    bx      lr
.endif
    .size m0_copy\bytes\()_\k, . - m0_copy\bytes\()_\k
.endm

.macro m0_copy_size bytes
    m0_copy_aligned \bytes
    m0_copy_shifted \bytes, 1
    m0_copy_shifted \bytes, 2
    m0_copy_shifted \bytes, 3
.endm

    m0_copy_size 16
    m0_copy_size 32
    m0_copy_size 64
    m0_copy_size 128
    m0_copy_size 256
    m0_copy_size 512
