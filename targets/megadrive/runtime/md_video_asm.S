/* Xtensa tile-column run for the Mega Drive index renderer (windowed ABI,
 * leaf).  The same semantics as span_pixels() in md_full_video.c: `n`
 * pixels of one pattern row, pattern columns from col0 stepping up
 * (forward) or down (reverse, H-flipped tile), never wrapping inside the
 * tile — the C caller splits runs at tile boundaries.  Every pixel stores
 * the tile's priority byte, like the C.  Pixels go out two per loaded
 * pattern byte (the two nibbles share it), the reason this beats the C.
 *
 * md_span_run(dst, pri, pat, pal, packed) with
 *   packed = col0 | (reverse << 4) | (pr << 5) | n << 8.
 * MD_VIDEO_ASM=0 in md_config.h keeps the C version; the boot selftest
 * (md_span_selftest) compares both on deterministic runs. */
    .text
    .align  4
    .global md_span_run
    .type   md_span_run, @function
md_span_run:
    entry   a1, 48
    /* a2 dst, a3 pri, a4 pat, a5 pal, a6 packed; a6/a7 are the temps */
    s32i    a6, a1, 24
    extui   a7, a6, 0, 3        /* col0 */
    extui   a6, a6, 5, 1
    s32i    a6, a1, 20          /* pr */
    l32i    a6, a1, 24
    bbsi    a6, 4, .Lrev       /* reverse bit set */

    /* forward: columns col0.., high nibble first in every byte */
    extui   a6, a6, 8, 4        /* n */
    s32i    a6, a1, 16
    extui   a8, a7, 1, 2        /* the byte of col0 */
    add     a4, a4, a8
    bbci    a7, 0, .Lf_aligned
    /* col0 odd: one lone low-nibble pixel, then byte-aligned pairs */
    l8ui    a7, a4, 0
    extui   a7, a7, 0, 4
    beqz    a7, 1f
    or      a7, a7, a5
1:  s8i     a7, a2, 0
    l32i    a7, a1, 20
    s8i     a7, a3, 0
    addi    a2, a2, 1
    addi    a3, a3, 1
    l32i    a6, a1, 16
    addi    a6, a6, -1
    s32i    a6, a1, 16
    beqz    a6, .Ldone
    addi    a4, a4, 1
.Lf_aligned:
    l32i    a6, a1, 16
    srli    a6, a6, 1           /* pair count */
.Lf_loop:
    beqz    a6, .Lf_tail
    addi    a6, a6, -1
    l8ui    a7, a4, 0
    extui   a7, a7, 4, 4
    beqz    a7, 2f
    or      a7, a7, a5
2:  s8i     a7, a2, 0
    l32i    a7, a1, 20
    s8i     a7, a3, 0
    l8ui    a7, a4, 0
    extui   a7, a7, 0, 4
    beqz    a7, 3f
    or      a7, a7, a5
3:  s8i     a7, a2, 1
    l32i    a7, a1, 20
    s8i     a7, a3, 1
    addi    a2, a2, 2
    addi    a3, a3, 2
    addi    a4, a4, 1
    j       .Lf_loop
.Lf_tail:
    l32i    a6, a1, 16
    bbci    a6, 0, .Ldone
    l8ui    a7, a4, 0
    extui   a7, a7, 4, 4
    beqz    a7, 4f
    or      a7, a7, a5
4:  s8i     a7, a2, 0
    l32i    a7, a1, 20
    s8i     a7, a3, 0
    j       .Ldone

    /* reverse: columns col0.., low nibble first in every byte */
.Lrev:
    extui   a6, a6, 8, 4
    s32i    a6, a1, 16
    extui   a8, a7, 1, 2        /* the byte of col0 */
    add     a4, a4, a8
    bbsi    a7, 0, .Lr_loop     /* col0 odd: pairs, low nibble first */
    /* col0 even: one lone high-nibble pixel, then pairs going down */
    l8ui    a7, a4, 0
    extui   a7, a7, 4, 4
    beqz    a7, 5f
    or      a7, a7, a5
5:  s8i     a7, a2, 0
    l32i    a7, a1, 20
    s8i     a7, a3, 0
    addi    a2, a2, 1
    addi    a3, a3, 1
    l32i    a6, a1, 16
    addi    a6, a6, -1
    s32i    a6, a1, 16
    beqz    a6, .Ldone
    addi    a4, a4, -1
.Lr_loop:
    l32i    a6, a1, 16
    srli    a6, a6, 1
.Lr_iter:
    beqz    a6, .Lr_tail
    addi    a6, a6, -1
    l8ui    a7, a4, 0
    extui   a7, a7, 0, 4
    beqz    a7, 6f
    or      a7, a7, a5
6:  s8i     a7, a2, 0
    l32i    a7, a1, 20
    s8i     a7, a3, 0
    l8ui    a7, a4, 0
    extui   a7, a7, 4, 4
    beqz    a7, 7f
    or      a7, a7, a5
7:  s8i     a7, a2, 1
    l32i    a7, a1, 20
    s8i     a7, a3, 1
    addi    a2, a2, 2
    addi    a3, a3, 2
    addi    a4, a4, -1
    j       .Lr_iter
.Lr_tail:
    l32i    a6, a1, 16
    bbci    a6, 0, .Ldone
    l8ui    a7, a4, 0
    extui   a7, a7, 0, 4
    beqz    a7, 8f
    or      a7, a7, a5
8:  s8i     a7, a2, 0
    l32i    a7, a1, 20
    s8i     a7, a3, 0
.Ldone:
    retw
