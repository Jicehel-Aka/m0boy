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
#include "gbl_timing_layout.h"

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

#if GBL_RENDER_LINE_ASM
/*
 * void gbl_ppu_render_line(void)
 *
 * render_line of gbl_ppu.c: window decision, sprite selection, the line
 * signature, then gbl_platform_line_check / line_begin / draw_line /
 * line_end.  In flash (it is large and calls C).  The sprites of the line
 * are sorted as 16-bit keys x << 8 | OAM index by insertion over a zero
 * sentinel, then narrowed to the index bytes draw_line takes.  Rare work
 * stays in C: the Y sort after an OAM change (gbl_ppu_sort_oam), more than
 * ten candidates (gbl_ppu_select_many), and the cached tile-version hash
 * (gbl_ppu_tiles_hash).  The M0 benchmark hashes every signature, so the
 * C and assembly signatures are compared line by line.
 *
 * Frame: sp+0/4 draw_line's stack arguments, sp+8 sel[10], sp+20 the
 * sentinel key, sp+22 keys[10], sp+44/48 saved values across calls.
 * r4 = signature, r5 = multiplier, r6 = gbl_io + 0x40, r7 = PPU state,
 * r8 = LCDC, r9 = window, r10 = sprite height, r11 = sprite count.
 */
#define RL_SEL 8
#define RL_SENTINEL 20                     /* keys[-1] */
#define RL_KEYS 22
#define RL_T0 44
#define RL_T1 48
#define RL_FRAME 52                        /* keeps sp 8-aligned at calls */

/* s = (s ^ r0) * k; s ^= s >> 15 (r0 clobbered) */
.macro SIG
    eors    r4, r0
    muls    r4, r5, r4
    lsrs    r0, r4, #15
    eors    r4, r0
.endm

    .section .text.gbl_ppu_render_line, "ax", %progbits
    .align 2
    .global gbl_ppu_render_line
    .type gbl_ppu_render_line, %function
    .thumb_func
gbl_ppu_render_line:
    push    {r4-r7, lr}
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    push    {r4-r7}
    sub     sp, #RL_FRAME
    ldr     r7, .Lrl_state
    ldr     r6, .Lrl_io_ppu
    ldrb    r0, [r6, #0x00]         /* LCDC */
    mov     r8, r0
    movs    r1, #0                  /* window: LCDC bits 0 and 5, LY >= WY, WX <= 166 */
    movs    r2, #0x21
    ands    r2, r0
    cmp     r2, #0x21
    bne     1f
    ldrb    r2, [r7, #GBL_P_LY]
    ldrb    r3, [r6, #0x0A]
    cmp     r2, r3
    blo     1f
    ldrb    r3, [r6, #0x0B]
    cmp     r3, #166
    bhi     1f
    movs    r1, #1
1:
    mov     r9, r1
    movs    r2, #8                  /* sprite height */
    lsls    r3, r0, #29
    bpl     2f
    movs    r2, #16
2:
    mov     r10, r2
    movs    r5, #0                  /* n */
    ldr     r3, .Lrl_skip
    ldrb    r3, [r3]
    cmp     r3, #0
    beq     3f
    b       .Lrl_tail
3:
    lsls    r3, r0, #30
    bmi     4f
    b       .Lrl_sig                /* sprites off */
4:
    /* ---- sprite selection ---- */
    ldr     r3, .Lrl_oam_dirty
    ldrb    r3, [r3]
    cmp     r3, #0
    beq     5f
    bl      gbl_ppu_sort_oam
5:
    ldrb    r0, [r7, #GBL_P_LY]
    movs    r1, #16
    adds    r1, r1, r0              /* ymax = ly + 16 */
    adds    r2, r1, #1
    mov     r3, r10
    subs    r2, r2, r3              /* ymin = ly + 17 - h */
    ldr     r4, .Lrl_ys
    movs    r3, #0                  /* lo */
    movs    r5, #0                  /* hi */
    cmp     r0, #0
    beq     6f
    subs    r0, #1
    ldrb    r3, [r7, #GBL_P_SPR_LY]
    cmp     r3, r0                  /* previous line's window ... */
    bne     7f
    ldrb    r3, [r7, #GBL_P_SPR_H]
    cmp     r3, r10                 /* ... for the same height */
    bne     7f
    ldrb    r3, [r7, #GBL_P_SPR_LO]
    ldrb    r5, [r7, #GBL_P_SPR_HI]
    b       6f
7:
    movs    r3, #0
6:
    cmp     r3, #40                 /* lo past Y < ymin */
    bhs     8f
    ldrb    r0, [r4, r3]
    cmp     r0, r2
    bhs     8f
    adds    r3, #1
    b       6b
8:
    cmp     r5, r3
    bhs     9f
    mov     r5, r3
9:
    ldrb    r0, [r4, r5]            /* hi past Y <= ymax (0xFF sentinel) */
    cmp     r0, r1
    bhi     10f
    adds    r5, #1
    b       9b
10:
    ldrb    r0, [r7, #GBL_P_LY]
    strb    r0, [r7, #GBL_P_SPR_LY]
    mov     r0, r10
    strb    r0, [r7, #GBL_P_SPR_H]
    strb    r3, [r7, #GBL_P_SPR_LO]
    strb    r5, [r7, #GBL_P_SPR_HI]
    subs    r2, r5, r3              /* candidates */
    cmp     r2, #10
    bls     11f
    add     r0, sp, #RL_SEL         /* more than ten: C */
    mov     r1, r3
    mov     r2, r5
    bl      gbl_ppu_select_many
    mov     r5, r0
    b       .Lrl_sig
11:
    ldr     r4, .Lrl_by_y
    adds    r4, r4, r3              /* candidates, Y order */
    adds    r3, r4, r2              /* end */
    ldr     r6, .Lrl_oam
    movs    r5, #0
    add     r0, sp, #RL_SENTINEL
    strh    r5, [r0]                /* sentinel key 0: not above any key */
12:
    cmp     r4, r3
    beq     15f
    ldrb    r0, [r4]                /* OAM index */
    adds    r4, #1
    lsls    r1, r0, #2
    adds    r1, r1, r6
    ldrb    r1, [r1, #1]            /* X */
    lsls    r1, r1, #8
    orrs    r1, r0                  /* key */
    lsls    r2, r5, #1
    add     r2, sp, r2
    adds    r2, #RL_KEYS            /* &keys[n] */
13:
    subs    r2, #2
    ldrh    r0, [r2]
    cmp     r0, r1
    bls     14f
    strh    r0, [r2, #2]
    b       13b
14:
    strh    r1, [r2, #2]
    adds    r5, #1
    b       12b
15:
    movs    r2, #0                  /* sel[j] = index byte of keys[j] */
    add     r1, sp, #RL_SENTINEL
    adds    r1, #2
    add     r3, sp, #RL_SEL
    b       17f
16:
    lsls    r0, r2, #1
    ldrh    r0, [r1, r0]
    strb    r0, [r3, r2]
    adds    r2, #1
17:
    cmp     r2, r5
    bne     16b
    ldr     r6, .Lrl_io_ppu

.Lrl_sig:  /* ---- line signature ---- */
    mov     r11, r5
    ldr     r5, .Lrl_mul
    ldr     r5, [r5]
    ldr     r4, .Lrl_fnv
    mov     r0, r8
    SIG
    mov     r0, r8
    lsls    r1, r0, #31
    bne     20f
    b       .Lrl_sig_obj            /* BG off */
20:
    ldrb    r1, [r6, #0x02]         /* y = SCY + LY */
    ldrb    r2, [r7, #GBL_P_LY]
    adds    r1, r1, r2
    uxtb    r1, r1
    lsrs    r3, r1, #3              /* mrow */
    lsls    r2, r0, #28
    bpl     21f
    adds    r3, #32
21:
    movs    r2, #20                 /* nbg: BG groups left of the window */
    mov     r0, r9
    cmp     r0, #0
    beq     23f
    ldrb    r2, [r6, #0x0B]
    subs    r2, #7
    bgt     22f
    movs    r2, #0
    b       23f
22:
    adds    r2, #7
    lsrs    r2, r2, #3
23:
    ldrb    r0, [r6, #0x07]         /* BGP */
    lsls    r0, r0, #24
    cmp     r2, #0
    beq     24f
    str     r3, [sp, #RL_T0]
    str     r2, [sp, #RL_T1]
    ldrb    r2, [r6, #0x03]         /* SCX */
    lsls    r2, r2, #16
    orrs    r0, r2
    lsls    r1, r1, #8
    orrs    r0, r1
    SIG
    ldr     r1, .Lrl_map_ver
    lsls    r0, r3, #1
    ldrh    r0, [r1, r0]
    SIG
    ldr     r0, [sp, #RL_T0]        /* tiles_hash(mrow, SCX >> 3, nbg + 1, LCDC) */
    ldrb    r1, [r6, #0x03]
    lsrs    r1, r1, #3
    ldr     r2, [sp, #RL_T1]
    adds    r2, #1
    mov     r3, r8
    bl      gbl_ppu_tiles_hash
24:
    SIG                             /* (BGP << 24 alone without BG groups) */
    mov     r0, r9
    cmp     r0, #0
    beq     .Lrl_sig_obj
    ldrb    r3, [r7, #GBL_P_WIN_LINE]
    lsrs    r2, r3, #3              /* wrow */
    mov     r0, r8
    lsls    r0, r0, #25
    bpl     25f
    adds    r2, #32
25:
    str     r2, [sp, #RL_T0]
    ldrb    r1, [r6, #0x0B]         /* WX */
    movs    r0, #21                 /* count */
    cmp     r1, #7
    blo     26f
    movs    r0, #174
    subs    r0, r0, r1
    lsrs    r0, r0, #3
    adds    r0, #1
    cmp     r0, #21
    bls     26f
    movs    r0, #21
26:
    str     r0, [sp, #RL_T1]
    lsls    r0, r1, #16
    lsls    r3, r3, #8
    orrs    r0, r3
    SIG
    ldr     r1, .Lrl_map_ver
    ldr     r0, [sp, #RL_T0]
    lsls    r0, r0, #1
    ldrh    r0, [r1, r0]
    SIG
    ldr     r0, [sp, #RL_T0]        /* tiles_hash(wrow, 0, count, LCDC | window) */
    movs    r1, #0
    ldr     r2, [sp, #RL_T1]
    movs    r3, #1
    lsls    r3, r3, #8
    add     r3, r8
    bl      gbl_ppu_tiles_hash
    SIG

.Lrl_sig_obj:
    mov     r0, r8
    lsls    r0, r0, #30
    bpl     .Lrl_check              /* sprites off */
    ldrb    r0, [r6, #0x08]         /* OBP0 << 8 | OBP1 */
    lsls    r0, r0, #8
    ldrb    r1, [r6, #0x09]
    orrs    r0, r1
    SIG
    movs    r3, #0
    b       31f
30:
    add     r1, sp, #RL_SEL
    ldrb    r1, [r1, r3]
    lsls    r1, r1, #2
    ldr     r0, .Lrl_oam
    adds    r1, r1, r0              /* OAM entry (word-aligned) */
    ldr     r0, [r1]
    SIG
    ldrb    r1, [r1, #2]            /* tile */
    ldr     r2, .Lrl_tile_ver
    mov     r0, r10
    cmp     r0, #16
    beq     32f
    lsls    r1, r1, #1
    ldrh    r0, [r2, r1]
    b       33f
32:
    lsrs    r1, r1, #1              /* 8x16: tiles t & 0xFE and t | 1 */
    lsls    r1, r1, #2
    ldr     r0, [r2, r1]            /* tile_ver[t] | tile_ver[t | 1] << 16 */
33:
    SIG
    adds    r3, #1
31:
    cmp     r3, r11
    blo     30b

.Lrl_check:
    ldrb    r0, [r7, #GBL_P_LY]
    mov     r1, r4
    bl      gbl_platform_line_check
    cmp     r0, #0
    beq     .Lrl_tail
    ldrb    r0, [r7, #GBL_P_LY]
    bl      gbl_platform_line_begin
    cmp     r0, #0
    beq     .Lrl_tail
    add     r1, sp, #RL_SEL         /* draw_line(out, lcdc, window, h, sel, n) */
    str     r1, [sp, #0]
    mov     r1, r11
    str     r1, [sp, #4]
    mov     r1, r8
    mov     r2, r9
    mov     r3, r10
    bl      gbl_ppu_draw_line       /* advances win_line */
    ldrb    r0, [r7, #GBL_P_LY]
    bl      gbl_platform_line_end
    b       .Lrl_ret
.Lrl_tail:
    mov     r0, r9
    cmp     r0, #0
    beq     .Lrl_ret
    ldrb    r0, [r7, #GBL_P_WIN_LINE]
    adds    r0, #1
    strb    r0, [r7, #GBL_P_WIN_LINE]
.Lrl_ret:
    add     sp, #RL_FRAME
    pop     {r4-r7}
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    pop     {r4-r7, pc}
    .align 2
.Lrl_state:     .word gbl_ppu_state
.Lrl_io_ppu:    .word gbl_io + 0x40
.Lrl_skip:      .word gbl_skip_render
.Lrl_oam_dirty: .word gbl_oam_dirty
.Lrl_ys:        .word gbl_ppu_y_sorted
.Lrl_by_y:      .word gbl_ppu_oam_by_y
.Lrl_oam:       .word gbl_oam
.Lrl_mul:       .word gbl_sig_mul
.Lrl_fnv:       .word 2166136261
.Lrl_map_ver:   .word gbl_ppu_map_ver
.Lrl_tile_ver:  .word gbl_ppu_tile_ver
    .size gbl_ppu_render_line, . - gbl_ppu_render_line
#endif

#endif
