/*
 * Thumb-1 (Cortex-M0+) versions of the functions that run on every device
 * event: gbl_ppu_advance, the PPU line state machine, gbl_update_evt, the
 * next-event computation, gbl_sync, and the LY/STAT path of gbl_io_rd.  Enabled by GBL_TIMING_ASM;
 * the C versions in gbl_ppu.c stay the reference and are used elsewhere.
 * Both must leave the machine in exactly the state the C leaves: the M0
 * benchmark checks it with -DBENCH_STATE_HASH -DBENCH_PIXEL_HASH.
 *
 * gbl_ppu_advance differs from the C loop in how it gets there:
 *  - one line is straight-line code (mode 3, render, mode 0, line end,
 *    mode 2), with no reload of stage and no dispatch between transitions;
 *  - the loop body is instantiated three times for the STAT line: with no
 *    source enabled (STAT bits 3-6 clear) every update is a no-op that
 *    leaves stat_line at 0 and the "quiet" copy omits them; with the LYC
 *    source alone every mode has the same line, so only line changes are
 *    checked; the general copy does every update like the C;
 *  - moved/evt_dirty are set once, stage/mode/line_start/next_stage_off are
 *    stored once on exit (mode follows from ly and stage);
 *  - a line of a skipped frame (gbl_skip_render) does not call render_line
 *    and its nine-register prologue: only the window line counter moves.
 */
#include "gbl_config.h"
#include "gbl_options.h"
#include "gbl_timing_layout.h"

#if GBL_TIMING_ASM && defined(__ARM_ARCH_6M__)

    .syntax unified
    .thumb

#define LINE_CYCLES 456
#define MODE3_START 80
#define MODE0_START 252

/* PPU registers relative to r5 = gbl_io + 0x40 (LDRB reaches 0-31). */
#define IO_LCDC 0x00
#define IO_STAT 0x01
#define IO_LYC 0x05
#define IO_WY 0x0A
#define IO_WX 0x0B
#define IO_IF_FROM_PPU 0x31        /* IF = r5 - 0x31 */

/* Registers in gbl_ppu_advance: r4 = &gbl_ppu_state, r5 = gbl_io + 0x40,
 * r6 = now, r7 = now - line_start, r8 = LINE_CYCLES.  r0-r3 are scratch
 * and do not survive the calls. */

/* IF |= bit (r0-r2 clobbered). */
.macro IF_SET bit
    mov     r2, r5
    subs    r2, #IO_IF_FROM_PPU
    ldrb    r1, [r2]
    movs    r0, #\bit
    orrs    r1, r0
    strb    r1, [r2]
.endm

/* stat_update() for a mode whose STAT source is bit `bit` of stat_modes
 * (bit 3 = mode 3: no source): a rising edge of the STAT line sets IF bit 1. */
.macro STAT_EDGE bit
    .if \bit == 3
    ldrb    r0, [r4, #GBL_P_LYC_LINE]
    .else
    ldrb    r0, [r4, #GBL_P_STAT_MODES]
    .if \bit
    lsrs    r0, r0, #\bit
    .endif
    movs    r1, #1
    ands    r0, r1
    ldrb    r1, [r4, #GBL_P_LYC_LINE]
    orrs    r0, r1
    .endif
    ldrb    r1, [r4, #GBL_P_STAT_LINE]
    strb    r0, [r4, #GBL_P_STAT_LINE]
    cmp     r0, r1
    bls     1f
    IF_SET  2
1:
.endm

/* lyc_refresh() for the new LY in r0: lyc_line = STAT bit 6 && LY == LYC. */
.macro LYC_REFRESH
    movs    r2, #0
    ldrb    r1, [r5, #IO_LYC]
    cmp     r0, r1
    bne     1f
    ldrb    r2, [r5, #IO_STAT]
    lsls    r2, r2, #25
    lsrs    r2, r2, #31
1:
    strb    r2, [r4, #GBL_P_LYC_LINE]
.endm

/* LYC-only STAT line for the new LY in r0 (STAT bits 3-5 clear, bit 6
 * set, stat_line == lyc_line on entry): every mode has the same line, so
 * only a line change can move it, and stat_line follows lyc_line. */
.macro LYC_ONLY
    ldrb    r1, [r5, #IO_LYC]
    cmp     r0, r1
    bne     1f
    ldrb    r1, [r4, #GBL_P_STAT_LINE]
    cmp     r1, #0
    bne     2f
    IF_SET  2
2:
    movs    r0, #1
    b       3f
1:
    movs    r0, #0
3:
    strb    r0, [r4, #GBL_P_LYC_LINE]
    strb    r0, [r4, #GBL_P_STAT_LINE]
.endm

/* Whole lines of a skipped frame (quiet and LYC-only copies), before the
 * normal path of .Ls0: each is mode 3, no rendering, mode 0 and a line
 * change whose STAT line update is a no-op (quiet) or sets lyc_line and
 * stat_line to 0 (LYC only, any line but the LYC line).  They are only
 * counted here; the line change into 144 or into the LYC line, and a line
 * not complete yet, are left to the normal path.  The window line moves
 * once per counted line at or below WY when the window is visible, as
 * render_line would move it. */
.macro SKIPPED_LINES q
    cmp     r7, r8
    blo     5f
    ldr     r0, .Lskip_render_\q
    ldrb    r0, [r0]
    cmp     r0, #0
    beq     5f
    ldrb    r0, [r4, #GBL_P_LY]
    mov     r3, r0                 /* first line */
    mov     r2, r8
    .if \q == 2
    ldrb    r1, [r5, #IO_LYC]
    .endif
6:
    adds    r0, #1                 /* the line after it */
    cmp     r0, #144
    beq     7f
    .if \q == 2
    cmp     r0, r1
    beq     7f
    .endif
    subs    r7, r7, r2
    cmp     r7, r2
    bhs     6b
    b       8f
7:
    subs    r0, #1                 /* left to the normal path */
8:  /* lines r3 .. r0 - 1 are done, r0 is the new LY */
    cmp     r0, r3
    beq     5f
    strb    r0, [r4, #GBL_P_LY]
    .if \q == 2
    movs    r1, #0
    strb    r1, [r4, #GBL_P_LYC_LINE]
    strb    r1, [r4, #GBL_P_STAT_LINE]
    .endif
    ldrb    r1, [r5, #IO_LCDC]
    movs    r2, #0x21
    ands    r1, r2
    cmp     r1, #0x21
    bne     5f
    ldrb    r1, [r5, #IO_WX]
    cmp     r1, #166
    bhi     5f
    ldrb    r1, [r5, #IO_WY]
    cmp     r1, r3
    bhs     9f
    mov     r1, r3                 /* max(first line, WY) */
9:
    subs    r1, r0, r1
    ble     5f
    ldrb    r2, [r4, #GBL_P_WIN_LINE]
    adds    r2, r2, r1
    strb    r2, [r4, #GBL_P_WIN_LINE]
5:
.endm

/* STAT line after a line change to the LY in r0, for a mode whose source
 * is bit `bit` of stat_modes. */
.macro LINE_STAT q, bit
    .if \q == 0
    LYC_REFRESH
    STAT_EDGE \bit
    .elseif \q == 2
    LYC_ONLY
    .endif
.endm

/* The line state machine, entered at the current LY and stage.  q selects
 * the STAT line handling: 0 general, 1 quiet (no source: every update is a
 * no-op and stat_line ends at 0), 2 LYC only. */
.macro PPU_LINES q
    ldrb    r1, [r4, #GBL_P_LY]
    cmp     r1, #144
    blo     1f
    b       .Lvbl_\q
1:
    ldrb    r0, [r4, #GBL_P_STAGE]
    cmp     r0, #1
    beq     .Ls1_\q
    bhi     .Ls2_\q
.Ls0_\q:                           /* LY < 144, before mode 3 */
    .if \q
    SKIPPED_LINES \q
    .endif
    cmp     r7, #MODE3_START
    blo     .Lx0_\q
    .if \q == 0
    STAT_EDGE 3
    .endif
.Ls1_\q:                           /* mode 3 */
    cmp     r7, #MODE0_START
    blo     .Lx1_\q
    ldr     r0, .Lskip_render_\q
    ldrb    r0, [r0]
    cmp     r0, #0
    bne     2f
    bl      gbl_ppu_render_line
    b       3f
2:  /* Skipped frame: render_line only advances the window line. */
    ldrb    r0, [r5, #IO_LCDC]
    movs    r1, #0x21
    ands    r0, r1
    cmp     r0, #0x21
    bne     3f
    ldrb    r1, [r5, #IO_WY]
    ldrb    r0, [r4, #GBL_P_LY]
    cmp     r0, r1
    blo     3f
    ldrb    r1, [r5, #IO_WX]
    cmp     r1, #166
    bhi     3f
    ldrb    r0, [r4, #GBL_P_WIN_LINE]
    adds    r0, #1
    strb    r0, [r4, #GBL_P_WIN_LINE]
3:
    .if \q == 0
    STAT_EDGE 0
    .endif
.Ls2_\q:                           /* mode 0 (HBlank) */
    cmp     r7, r8
    blo     .Lx2_\q
    mov     r0, r8
    subs    r7, r7, r0
    ldrb    r0, [r4, #GBL_P_LY]
    adds    r0, #1
    cmp     r0, #144
    beq     .Lvbl_enter_\q
    strb    r0, [r4, #GBL_P_LY]
    LINE_STAT \q, 2
    b       .Ls0_\q

/* Exits: stage, mode and the offset of the next transition. */
.Lx0_\q:
    movs    r0, #0
    movs    r1, #2
    movs    r2, #MODE3_START
    b       .Lexit_\q
.Lx1_\q:
    movs    r0, #1
    movs    r1, #3
    movs    r2, #MODE0_START
    b       .Lexit_\q
.Lx2_\q:
    movs    r0, #2
    movs    r1, #0
    mov     r2, r8
    b       .Lexit_\q
.Lxv_\q:
    movs    r0, #0
    movs    r1, #1
    mov     r2, r8
.Lexit_\q:
    .if \q == 1
    movs    r3, #0
    strb    r3, [r4, #GBL_P_STAT_LINE]
    .endif
    b       .Lexit

.Lvbl_enter_\q:                    /* line 144: VBlank */
    strb    r0, [r4, #GBL_P_LY]
    .if \q == 0
    LYC_REFRESH
    .endif
    IF_SET  1
    .if \q == 0
    STAT_EDGE 1
    .elseif \q == 2
    movs    r0, #144
    LYC_ONLY                       /* IF bits commute: same IF as after */
    .endif
    ldr     r0, .Lframe_\q
    ldr     r1, [r0]
    adds    r1, #1
    str     r1, [r0]
    /* The frontend sees the state the C loop has at this point. */
    subs    r0, r6, r7
    str     r0, [r4, #GBL_P_LINE_START]
    movs    r0, #0
    strb    r0, [r4, #GBL_P_STAGE]
    .if \q == 1
    strb    r0, [r4, #GBL_P_STAT_LINE]
    .endif
    movs    r0, #1
    strb    r0, [r4, #GBL_P_MODE]
    bl      gbl_platform_vblank
.Lvbl_\q:                          /* LY 144-153 */
    cmp     r7, r8
    blo     .Lxv_\q
    mov     r0, r8
    subs    r7, r7, r0
    ldrb    r0, [r4, #GBL_P_LY]
    adds    r0, #1
    cmp     r0, #154
    beq     4f
    strb    r0, [r4, #GBL_P_LY]
    LINE_STAT \q, 1
    b       .Lvbl_\q
4:  /* line 0 of the next frame */
    movs    r0, #0
    strb    r0, [r4, #GBL_P_LY]
    strb    r0, [r4, #GBL_P_WIN_LINE]
#if GBL_FRAME_SIGNATURE
    bl      gbl_ppu_frame_start
#endif
    movs    r0, #0
    LINE_STAT \q, 2
    b       .Ls0_\q
    .align 2                       /* literals within forward reach (LDR literal) */
.Lskip_render_\q:  .word gbl_skip_render
.Lframe_\q:        .word gbl_frame

.endm

/*
 * void gbl_ppu_advance(uint32_t now)
 */
    .section .text.gbl_ppu_advance, "ax", %progbits
    .align 2
    .global gbl_ppu_advance
    .type gbl_ppu_advance, %function
    .thumb_func
gbl_ppu_advance:
    ldr     r3, .Lstate
    ldrb    r1, [r3, #GBL_P_LCD_ON]
    cmp     r1, #0
    bne     2f
    ldr     r1, .Loff_fn           /* LCD off: the C virtual frame clock */
    bx      r1
2:
    ldr     r1, [r3, #GBL_P_LINE_START]
    subs    r1, r0, r1
    ldr     r2, [r3, #GBL_P_NEXT_STAGE_OFF]
    cmp     r1, r2
    bhs     1f
    bx      lr                     /* no transition due: nothing changes */
1:
    push    {r4-r7, lr}
    mov     r4, r8
    push    {r4}
    movs    r2, #1
    strb    r2, [r3, #GBL_P_MOVED]
    strb    r2, [r3, #GBL_P_EVT_DIRTY]
    mov     r4, r3
    ldr     r5, .Lio_ppu
    mov     r6, r0
    mov     r7, r1
    ldr     r0, .Lline_cycles
    mov     r8, r0
    /* Quiet when no STAT source is enabled: stat_modes (bits 3-5) is 0 and
     * so is lyc_line, since STAT bit 6 is clear. */
    ldrb    r0, [r5, #IO_STAT]
    movs    r1, #0x78
    tst     r0, r1
    beq     .Lquiet
    movs    r1, #0x38              /* STAT bit 6 only, and the line in step */
    tst     r0, r1
    bne     1f
    ldrb    r0, [r4, #GBL_P_STAT_LINE]
    ldrb    r1, [r4, #GBL_P_LYC_LINE]
    cmp     r0, r1
    bne     1f
    b       .Llyc
1:
    b       .Lloud
    .align 2
.Lstate:        .word gbl_ppu_state
.Lio_ppu:       .word gbl_io + 0x40
.Lline_cycles:  .word LINE_CYCLES
.Loff_fn:       .word gbl_ppu_advance_off
.Lquiet:
    PPU_LINES 1
.Llyc:
    PPU_LINES 2
.Lloud:
    PPU_LINES 0

.Lexit:  /* r0 = stage, r1 = mode, r2 = next_stage_off */
    strb    r0, [r4, #GBL_P_STAGE]
    strb    r1, [r4, #GBL_P_MODE]
    str     r2, [r4, #GBL_P_NEXT_STAGE_OFF]
    subs    r3, r6, r7             /* line_start = now - off */
    str     r3, [r4, #GBL_P_LINE_START]
    adds    r3, r3, r2
    ldr     r0, .Ldue
    str     r3, [r0]
    pop     {r4}
    mov     r8, r4
    pop     {r4-r7, pc}

    .align 2
.Ldue:          .word gbl_ppu_due
    .size gbl_ppu_advance, . - gbl_ppu_advance

/*
 * void gbl_update_evt(void)
 * Pending enabled interrupt: next_evt = cyc.  Otherwise the cached device
 * event, recomputed when dirty (ppu_next_event of gbl_ppu.c, then the
 * timer/serial cache of gbl_core_next_event, calling it only when that
 * cache is dirty too), bounded to one frame ahead.  The usual case needs
 * no stack frame.
 */
    .section .text.gbl_update_evt, "ax", %progbits
    .align 2
    .global gbl_update_evt
    .type gbl_update_evt, %function
    .thumb_func
gbl_update_evt:
    ldr     r3, .Le_gbr
    ldrb    r0, [r3, #GBL_GBR_IME]
    cmp     r0, #0
    beq     1f
    ldr     r1, .Le_ie
    ldrb    r1, [r1]
    ldr     r2, .Le_io
    ldrb    r2, [r2, #0x0F]         /* IF */
    ands    r1, r2
    lsls    r1, r1, #27
    beq     1f
    ldr     r0, [r3, #GBL_GBR_CYC]
    str     r0, [r3, #GBL_GBR_NEXT_EVT]
    bx      lr
1:
    ldr     r2, .Le_state
    ldrb    r0, [r2, #GBL_P_EVT_DIRTY]
    cmp     r0, #0
    bne     .Le_recompute
.Le_bound:  /* r2 = PPU state, r3 = gbr */
    ldr     r0, [r2, #GBL_P_DEV_NEXT]
    ldr     r1, [r3, #GBL_GBR_CYC]
    ldr     r2, .Le_frame
    subs    r0, r0, r1
    cmp     r0, r2
    ble     2f
    mov     r0, r2
2:
    adds    r0, r0, r1
    str     r0, [r3, #GBL_GBR_NEXT_EVT]
    bx      lr

.Le_recompute:
    push    {r4, r5, lr}
    ldrb    r0, [r2, #GBL_P_LCD_ON]
    cmp     r0, #0
    bne     3f
    ldr     r0, [r2, #GBL_P_OFF_START]  /* LCD off: next virtual frame */
    ldr     r1, .Le_frame
    adds    r0, r0, r1
    b       .Le_core
3:
    ldr     r4, .Le_io_ppu
    ldrb    r1, [r4, #0x01]         /* STAT */
    ldrb    r5, [r2, #GBL_P_LY]
    ldr     r0, [r2, #GBL_P_LINE_START]
    cmp     r5, #144
    bhs     4f
    ldrb    r3, [r2, #GBL_P_STAGE]
    cmp     r3, #2
    bhs     4f
    lsls    r3, r1, #28
    bpl     4f
    adds    r0, #252                /* mode 0 interrupt: next HBlank */
    b       .Le_core
4:
#if GBL_PPU_BATCH > 1
    lsls    r3, r1, #26
    bpl     5f
#endif
    ldr     r3, .Le_line
    adds    r0, r0, r3              /* mode 2 interrupt: next line */
    b       .Le_core
#if GBL_PPU_BATCH > 1
5:  /* batch end, VBlank, the LYC line or the next frame */
    movs    r3, #154
    cmp     r5, #144
    bhs     6f
    movs    r3, #(GBL_PPU_BATCH - 1)
    orrs    r3, r5
    adds    r3, #1
    cmp     r3, #144
    bls     6f
    movs    r3, #144
6:
    lsls    r1, r1, #25
    bpl     7f
    ldrb    r1, [r4, #0x05]         /* LYC */
    cmp     r1, r5
    bls     7f
    cmp     r1, r3
    bhs     7f
    mov     r3, r1
7:
    subs    r3, r3, r5
    ldr     r1, .Le_line
    muls    r3, r1, r3
    adds    r0, r0, r3
#endif
.Le_core:  /* r0 = PPU event; the timer/serial event if it comes first */
    ldr     r1, .Le_timer
    ldrb    r3, [r1, #GBL_T_CORE_DIRTY]
    cmp     r3, #0
    bne     9f
    ldrb    r3, [r1, #GBL_T_CORE_HAS_NEXT]
    cmp     r3, #0
    beq     8f
    ldr     r3, [r1, #GBL_T_CORE_NEXT]
    subs    r4, r3, r0
    bpl     8f
    mov     r0, r3
    b       8f
9:
    bl      gbl_core_next_event
8:
    ldr     r2, .Le_state
    str     r0, [r2, #GBL_P_DEV_NEXT]
    movs    r1, #0
    strb    r1, [r2, #GBL_P_EVT_DIRTY]
    ldr     r3, .Le_gbr
    pop     {r4, r5}
    pop     {r1}
    mov     lr, r1
    b       .Le_bound
    .align 2
.Le_frame:  .word 70224
.Le_line:   .word LINE_CYCLES
.Le_gbr:    .word gbr
.Le_ie:     .word gbl_ie
.Le_io:     .word gbl_io
.Le_io_ppu: .word gbl_io + 0x40
.Le_state:  .word gbl_ppu_state
.Le_timer:  .word gbl_timer_state
    .size gbl_update_evt, . - gbl_update_evt

/*
 * void gbl_sync(void)
 * Devices to gbr.cyc: the PPU (when due), the timer, a serial transfer in
 * progress, then the next event.  The timer is advanced here unless TIMA
 * overflows (then the C loop, gbl_timer_sync); GCC's version saved nine
 * registers before finding that nothing had ticked.
 */
    .section .text.gbl_sync, "ax", %progbits
    .align 2
    .global gbl_sync
    .type gbl_sync, %function
    .thumb_func
gbl_sync:
    push    {r4, r5, lr}
    ldr     r3, 5f
    ldr     r4, [r3, #GBL_GBR_CYC]  /* now */
    ldr     r2, 6f
    ldr     r2, [r2]
    subs    r2, r4, r2
    bmi     1f
    movs    r0, r4
    bl      gbl_ppu_advance
1:
    ldr     r3, 7f
    ldrb    r0, [r3, #0x07]         /* TAC */
    ldr     r2, 8f
    lsls    r1, r0, #29
    bpl     3f                      /* timer stopped: only tima_sync moves */
    lsls    r0, r0, #30
    lsrs    r0, r0, #30
    ldr     r1, 9f
    ldrb    r1, [r1, r0]            /* shift of the TAC clock */
    ldr     r0, [r2, #GBL_T_DIV_BASE]
    ldr     r5, [r2, #GBL_T_TIMA_SYNC]
    subs    r5, r5, r0
    lsrs    r5, r1
    subs    r0, r4, r0
    lsrs    r0, r1
    subs    r0, r0, r5              /* ticks */
    beq     3f
    ldrb    r1, [r3, #0x05]         /* TIMA */
    adds    r1, r1, r0
    cmp     r1, #255
    bhi     2f
    strb    r1, [r3, #0x05]
    b       3f
2:
    movs    r0, r4                  /* overflow: reload, IF, event */
    bl      gbl_timer_sync
    b       4f
3:
    str     r4, [r2, #GBL_T_TIMA_SYNC]
4:
    ldr     r2, 8f
    ldrb    r0, [r2, #GBL_T_SERIAL_BUSY]
    cmp     r0, #0
    bne     10f
    bl      gbl_update_evt
    pop     {r4, r5, pc}
10:
    movs    r0, r4
    bl      gbl_sync_serial
    pop     {r4, r5, pc}
    .align 2
5:  .word   gbr
6:  .word   gbl_ppu_due
7:  .word   gbl_io
8:  .word   gbl_timer_state
9:  .word   gbl_tac_shift
    .size gbl_sync, . - gbl_sync

/*
 * uint8_t gbl_io_rd(uint8_t n)
 * PPU registers like gbl_ppu_read: for LY and STAT the PPU is advanced
 * when a transition is due (then the next event) and the value is built
 * here; the others read back as written.  Other registers go to the C
 * gbl_io_rd_c.
 */
    .section .text.gbl_io_rd, "ax", %progbits
    .align 2
    .global gbl_io_rd
    .type gbl_io_rd, %function
    .thumb_func
gbl_io_rd:
    movs    r1, r0
    subs    r1, #0x40
    cmp     r1, #0x0B
    bhi     4f                      /* not a PPU register: C */
    cmp     r0, #0x44
    beq     1f
    cmp     r0, #0x41
    beq     1f
    ldr     r1, .Lr_io              /* other PPU registers read back as written */
    ldrb    r0, [r1, r0]
    bx      lr
4:
    ldr     r1, .Lr_c
    bx      r1
1:
    push    {r4, lr}
    mov     r4, r0
    ldr     r3, .Lr_state
    movs    r1, #0
    strb    r1, [r3, #GBL_P_MOVED]
    ldr     r2, .Lr_gbr
    ldr     r0, [r2, #GBL_GBR_CYC]
    ldr     r1, .Lr_due
    ldr     r1, [r1]
    subs    r1, r0, r1
    bmi     2f
    bl      gbl_ppu_advance
    ldr     r3, .Lr_state
    ldrb    r0, [r3, #GBL_P_MOVED]
    cmp     r0, #0
    beq     2f
    bl      gbl_update_evt
2:
    ldr     r3, .Lr_state
    ldrb    r1, [r3, #GBL_P_LCD_ON]
    cmp     r4, #0x44
    bne     3f
    movs    r0, #0                  /* LY reads 0 while the LCD is off */
    cmp     r1, #0
    beq     9f
    ldrb    r0, [r3, #GBL_P_LY]
    pop     {r4, pc}
3:  /* STAT: bit 7, the sources, and with the LCD on the coincidence and mode */
    ldr     r2, .Lr_io_ppu
    ldrb    r0, [r2, #0x01]
    movs    r4, #0x78
    ands    r0, r4
    adds    r0, #0x80
    cmp     r1, #0
    beq     9f
    ldrb    r4, [r3, #GBL_P_MODE]
    orrs    r0, r4
    ldrb    r4, [r3, #GBL_P_LY]
    ldrb    r1, [r2, #0x05]         /* LYC */
    cmp     r4, r1
    bne     9f
    adds    r0, #4
9:
    pop     {r4, pc}
    .align 2
.Lr_c:      .word gbl_io_rd_c
.Lr_state:  .word gbl_ppu_state
.Lr_gbr:    .word gbr
.Lr_due:    .word gbl_ppu_due
.Lr_io_ppu: .word gbl_io + 0x40
.Lr_io:     .word gbl_io
    .size gbl_io_rd, . - gbl_io_rd

#endif
