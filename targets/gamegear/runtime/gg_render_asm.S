/* Game Gear background tiles, Thumb-1 (Cortex-M0+), from SRAM: the tile
 * loop of background() in gg_vdp.c, same results.
 *
 * unsigned gg_bg_tiles(uint16_t *out, const uint8_t *names_row, unsigned tx,
 *                      const gg_bg_args *args)
 * args: 0 vram, 4 tr (row in the tile), 8 pal (CRAM as RGB565), 12 planes
 * (gg_planes_tab: the flipped table 1 KiB further), 16 count.
 * Draws count tiles of 8 pixels from name column tx (wrapping at 32) and
 * returns the tiles (bit k) with priority and a non-zero pixel.
 *
 * Per tile: name entry, row bytes, four table loads spreading the planes
 * into eight 4-bit fields (w); per pixel two ALU operations give the
 * palette offset (field * 2), then a halfword load and store.  A tile with
 * the same name entry as the previous one (same row: the same pixels)
 * copies its eight pixels instead. */
#include "sg_config.h"

#if defined(__ARM_ARCH_6M__) && !defined(GG_NO_RENDER_ASM)

    .syntax unified
    .thumb

/* SRAM (no flash wait states) unless the target asks for flash
 * (SG_CODE_IN_SRAM 0: the META, where SRAM code faulted overclocked). */
#if !defined(SG_CODE_IN_SRAM) || SG_CODE_IN_SRAM
#define GG_RAMFUNC(name) .section .data.gg_ramfunc.name, "ax", %progbits
#else
#define GG_RAMFUNC(name) .section .text.gg_ramfunc.name, "ax", %progbits
#endif

    GG_RAMFUNC(gg_bg_tiles)
    .align 2
    .global gg_bg_tiles
    .type gg_bg_tiles, %function
    .thumb_func
gg_bg_tiles:
    push {r4-r7, lr}
    mov r4, r8
    mov r5, r9
    mov r6, r10
    mov r7, r11
    push {r4-r7}
    ldr r4, [r3, #0]
    mov r8, r4                 /* vram */
    ldr r4, [r3, #4]
    mov r9, r4                 /* tr */
    ldr r4, [r3, #8]
    mov r10, r4                /* pal */
    ldr r4, [r3, #12]
    mov r11, r4                /* planes */
    ldr r4, [r3, #16]          /* count -> end of out */
    lsls r4, r4, #4
    adds r4, r4, r0
    mov lr, r4
    movs r4, #1
    mov r12, r4                /* bit of this tile */
    movs r4, #0
    mvns r5, r4
    push {r4, r5}              /* priority tiles at [sp], previous entry at [sp, #4] */
1:  lsls r5, r2, #1
    ldrh r5, [r1, r5]          /* name entry */
    ldr r4, [sp, #4]
    cmp r4, r5
    beq 6f                     /* repeated: w and r6 still hold its fields */
    str r5, [sp, #4]
    mov r6, r9
    lsls r4, r5, #21           /* V flip (bit 10) */
    bpl 2f
    movs r4, #7
    subs r6, r4, r6
2:  lsls r4, r5, #23
    lsrs r4, r4, #18           /* tile * 32 (below 16 KiB) */
    lsls r6, r6, #2
    adds r4, r4, r6
    add r4, r8                 /* row bytes */
    mov r6, r11
    lsls r7, r5, #22           /* H flip (bit 9): the other table */
    bpl 3f
    movs r7, #1
    lsls r7, r7, #10
    adds r6, r6, r7
3:  ldrb r7, [r4, #0]
    lsls r7, r7, #2
    ldr r3, [r6, r7]           /* w = T[p0] */
    ldrb r7, [r4, #1]
    lsls r7, r7, #2
    ldr r7, [r6, r7]
    lsls r7, r7, #1
    orrs r3, r7                /* | T[p1] << 1 */
    ldrb r7, [r4, #2]
    lsls r7, r7, #2
    ldr r7, [r6, r7]
    lsls r7, r7, #2
    orrs r3, r7                /* | T[p2] << 2 */
    ldrb r7, [r4, #3]
    lsls r7, r7, #2
    ldr r7, [r6, r7]
    lsls r7, r7, #3
    orrs r3, r7                /* | T[p3] << 3 */
    mov r6, r10
    lsls r4, r5, #20           /* sprite palette (bit 11) */
    bpl 4f
    adds r6, #32
4:  lsls r4, r5, #19           /* priority (bit 12) with a pixel */
    bpl 5f
    cmp r3, #0
    beq 5f
    ldr r4, [sp]
    mov r7, r12
    orrs r4, r7
    str r4, [sp]
5:  movs r7, #0x1e
    lsls r4, r3, #1
    ands r4, r7
    ldrh r4, [r6, r4]
    strh r4, [r0, #0]
    lsrs r4, r3, #3
    ands r4, r7
    ldrh r4, [r6, r4]
    strh r4, [r0, #2]
    lsrs r4, r3, #7
    ands r4, r7
    ldrh r4, [r6, r4]
    strh r4, [r0, #4]
    lsrs r4, r3, #11
    ands r4, r7
    ldrh r4, [r6, r4]
    strh r4, [r0, #6]
    lsrs r4, r3, #15
    ands r4, r7
    ldrh r4, [r6, r4]
    strh r4, [r0, #8]
    lsrs r4, r3, #19
    ands r4, r7
    ldrh r4, [r6, r4]
    strh r4, [r0, #10]
    lsrs r4, r3, #23
    ands r4, r7
    ldrh r4, [r6, r4]
    strh r4, [r0, #12]
    lsrs r4, r3, #27
    ands r4, r7
    ldrh r4, [r6, r4]
    strh r4, [r0, #14]
7:  adds r0, #16
    adds r2, r2, #1
    movs r4, #31
    ands r2, r4                /* next name column */
    mov r4, r12
    lsls r4, r4, #1
    mov r12, r4
    cmp r0, lr
    bne 1b
    pop {r0, r1}               /* result */
    pop {r4-r7}
    mov r8, r4
    mov r9, r5
    mov r10, r6
    mov r11, r7
    pop {r4-r7, pc}
6:  lsls r4, r5, #19           /* priority as for the previous tile */
    bpl 8f
    cmp r3, #0
    beq 8f
    ldr r4, [sp]
    mov r7, r12
    orrs r4, r7
    str r4, [sp]
8:  mov r4, r0                 /* the previous tile's pixels */
    subs r4, #16
    ldrh r7, [r4, #0]
    strh r7, [r0, #0]
    ldrh r7, [r4, #2]
    strh r7, [r0, #2]
    ldrh r7, [r4, #4]
    strh r7, [r0, #4]
    ldrh r7, [r4, #6]
    strh r7, [r0, #6]
    ldrh r7, [r4, #8]
    strh r7, [r0, #8]
    ldrh r7, [r4, #10]
    strh r7, [r0, #10]
    ldrh r7, [r4, #12]
    strh r7, [r0, #12]
    ldrh r7, [r4, #14]
    strh r7, [r0, #14]
    b 7b
    .size gg_bg_tiles, . - gg_bg_tiles

/* Master System background: unsigned gg_sms_bg(uint16_t *out, const
 * gg_sms_args *a) -- sms_bg_fast in gg_vdp.c, same results.  For each of
 * 33 tiles: name entry, row fields (as above); then group k - 1 (the end of
 * tile k - 1 and the start of tile k when finely scrolled) gives five
 * pixels: four from the pair table of its palette, one mean of two spread
 * colours.  Groups across two palettes are returned (bit g) for the C path.
 * args (gg_sms_args): 0 names row, 4 locked names row, 8 vram + tr * 4,
 * 12 the same for the locked row, 16 / 20 the same for a flipped tile,
 * 24 tiles drawn from the first row (33: no lock), 28 2 * first tile column,
 * 32 sh (4 * fine), 36 32 - sh, 40 planes, 44 pairs (palette 1 at +512),
 * 48 spal (palette 1 at +64).
 * Two phases (the lock splits the line), the loop itself has no per-tile
 * test but its counter: r4 names row, r6 offset in it (wraps at 64),
 * r8 previous fields, r9 previous palette (2: no tile yet), r10 slow groups,
 * r11 vram + tr * 4, r12 previous name entry, lr tiles left.
 * Stack: [0] fields and [4] palette of the last group drawn here (palette
 * 2: none, its pixels are not at out - 10), 8 args, 12 out, 16 phase,
 * 20 flipped row, 24 planes, 28 sh, 32 32 - sh, 36 pairs, 40 spal, 44 the
 * previous tile was a repeat too (its group is already drawn).
 * A tile repeating the previous one reuses its fields; a group equal to the
 * last one copies its five pixels. */
    GG_RAMFUNC(gg_sms_bg)
    .align 2
    .global gg_sms_bg
    .type gg_sms_bg, %function
    .thumb_func
gg_sms_bg:
    push {r4-r7, lr}
    mov r4, r8
    mov r5, r9
    mov r6, r10
    mov r7, r11
    push {r4-r7}
    sub sp, #48
    str r1, [sp, #8]
    str r0, [sp, #12]
    movs r2, #2
    str r2, [sp, #4]           /* no group drawn */
    mov r9, r2                 /* no previous tile */
    movs r2, #0
    str r2, [sp, #16]          /* phase 0 */
    str r2, [sp, #44]          /* no repeated tile yet */
    mov r8, r2
    mov r10, r2
    mvns r2, r2
    mov r12, r2                /* no previous entry */
    ldr r2, [r1, #16]
    str r2, [sp, #20]
    ldr r2, [r1, #40]
    str r2, [sp, #24]
    ldr r2, [r1, #32]
    str r2, [sp, #28]
    ldr r2, [r1, #36]
    str r2, [sp, #32]
    ldr r2, [r1, #44]
    str r2, [sp, #36]
    ldr r2, [r1, #48]
    str r2, [sp, #40]
    ldr r4, [r1, #0]
    ldr r6, [r1, #28]
    ldr r2, [r1, #8]
    mov r11, r2
    ldr r2, [r1, #24]
    mov lr, r2
    b 1f
30: ldr r1, [sp, #44]          /* the previous tile again */
    cmp r1, #0
    bne 35f                    /* the second time: the group repeats too */
    movs r1, #1
    str r1, [sp, #44]
    mov r7, r8                 /* its fields and palette */
    mov r3, r9
    b 5f
35: mov r7, r0                 /* a run of the same tile: its group, five pixels */
    subs r7, #10               /* from out - 10, kept in registers */
    ldrh r1, [r7, #0]
    ldrh r2, [r7, #2]
    lsls r2, r2, #16
    orrs r1, r2
    ldrh r5, [r7, #4]
    ldrh r2, [r7, #6]
    lsls r2, r2, #16
    orrs r5, r2
    ldrh r7, [r7, #8]
    b 37f
36: ldrh r3, [r4, r6]          /* the next name entry */
    adds r6, #2
    movs r2, #63
    ands r6, r2
    cmp r3, r12
    bne 50f                    /* another tile: back to the loop */
37: strh r1, [r0, #0]
    lsrs r2, r1, #16
    strh r2, [r0, #2]
    strh r5, [r0, #4]
    lsrs r2, r5, #16
    strh r2, [r0, #6]
    strh r7, [r0, #8]
    adds r0, #10
    mov r2, lr
    subs r2, #1
    mov lr, r2
    bne 36b
    b 51f                      /* the row is done */
31: mov r1, r11                /* a flipped tile: the row from the bottom */
    subs r2, r2, r1
    ldr r1, [sp, #20]
    adds r2, r2, r1
    b 2f
32: movs r1, #1
    lsls r1, r1, #10
    adds r5, r5, r1
    b 3f
33: mov r1, r9                 /* two palettes */
    cmp r1, #2
    beq 34f                    /* the first tile: no group */
    ldr r1, [sp, #28]
    cmp r1, #0
    beq 5f                     /* not scrolled: the group is the tile */
    ldr r1, [sp, #12]          /* its group number: (out - first) / 10 */
    subs r1, r0, r1
    ldr r2, =3277
    muls r1, r2
    lsrs r1, r1, #15
    movs r2, #1
    lsls r2, r1
    mov r1, r10
    orrs r1, r2
    mov r10, r1
    movs r1, #2
    str r1, [sp, #4]           /* its pixels come later, from C */
    mov r8, r7
    mov r9, r3
    b 7f
34: mov r8, r7
    mov r9, r3
    b 8f
22: mov r7, r0                 /* the last group's pixels */
    subs r7, #10
    ldrh r1, [r7, #0]
    strh r1, [r0, #0]
    ldrh r1, [r7, #2]
    strh r1, [r0, #2]
    ldrh r1, [r7, #4]
    strh r1, [r0, #4]
    ldrh r1, [r7, #6]
    strh r1, [r0, #6]
    ldrh r1, [r7, #8]
    strh r1, [r0, #8]
    b 7f
1:  ldrh r3, [r4, r6]          /* name entry */
    adds r6, #2
    movs r1, #63
    ands r6, r1                /* the row wraps */
    cmp r3, r12
    beq 30b                    /* the previous tile again: its fields */
50: mov r12, r3
    movs r1, #0
    str r1, [sp, #44]          /* not a repeat */
    lsls r2, r3, #23
    lsrs r2, r2, #18           /* tile * 32 */
    add r2, r11                /* row bytes */
    lsls r1, r3, #21           /* V flip */
    bmi 31b
2:  ldr r5, [sp, #24]
    lsls r1, r3, #22           /* H flip: the other table */
    bmi 32b
3:  ldrb r1, [r2, #0]
    lsls r1, r1, #2
    ldr r7, [r5, r1]
    ldrb r1, [r2, #1]
    lsls r1, r1, #2
    ldr r1, [r5, r1]
    lsls r1, r1, #1
    orrs r7, r1
    ldrb r1, [r2, #2]
    lsls r1, r1, #2
    ldr r1, [r5, r1]
    lsls r1, r1, #2
    orrs r7, r1
    ldrb r1, [r2, #3]
    lsls r1, r1, #2
    ldr r1, [r5, r1]
    lsls r1, r1, #3
    orrs r7, r1                /* fields of tile k */
4:  lsls r3, r3, #20
    lsrs r3, r3, #31           /* its palette */
    cmp r9, r3
    bne 33b
5:  ldr r1, [sp, #28]          /* the group: end of the previous tile, */
    mov r5, r8
    lsrs r5, r1
    ldr r1, [sp, #32]
    mov r2, r7
    lsls r2, r1                /* start of this one (sh 0: 32 gives 0) */
    orrs r5, r2
    mov r2, r9                 /* the group's palette */
    mov r8, r7
    mov r9, r3
    ldr r1, [sp, #0]
    cmp r1, r5
    bne 6f
    ldr r1, [sp, #4]
    cmp r1, r2
    bne 6f
    b 22b                      /* the last group again: copy it */
6:  str r5, [sp, #0]
    str r2, [sp, #4]
    lsls r7, r2, #9
    ldr r1, [sp, #36]
    adds r7, r7, r1            /* pairs of its palette */
    lsls r3, r2, #6
    ldr r1, [sp, #40]
    adds r3, r3, r1            /* spal of its palette */
    lsls r1, r5, #24
    lsrs r1, r1, #23
    ldrh r1, [r7, r1]
    strh r1, [r0, #0]
    lsls r1, r5, #20
    lsrs r1, r1, #28
    lsls r1, r1, #1
    lsls r2, r5, #24
    lsrs r2, r2, #28
    lsls r2, r2, #5
    orrs r1, r2
    ldrh r1, [r7, r1]
    strh r1, [r0, #2]
    lsls r1, r5, #16
    lsrs r1, r1, #28
    lsls r1, r1, #2
    ldr r1, [r3, r1]
    lsls r2, r5, #12
    lsrs r2, r2, #28
    lsls r2, r2, #2
    ldr r2, [r3, r2]
    adds r1, r1, r2
    lsrs r1, r1, #1
    ldr r2, =0x07e0f81f
    ands r1, r2
    lsrs r2, r1, #16
    orrs r1, r2
#if GG_PANEL_SWAP
    rev16 r1, r1
#endif
    strh r1, [r0, #4]
    lsls r1, r5, #4
    lsrs r1, r1, #24
    lsls r1, r1, #1
    ldrh r1, [r7, r1]
    strh r1, [r0, #6]
    lsrs r1, r5, #28
    lsls r1, r1, #1
    lsls r2, r5, #4
    lsrs r2, r2, #28
    lsls r2, r2, #5
    orrs r1, r2
    ldrh r1, [r7, r1]
    strh r1, [r0, #8]
7:  adds r0, #10
8:  mov r1, lr
    subs r1, #1
    mov lr, r1
    bne 1b
51: ldr r1, [sp, #16]          /* the first row done: the locked one */
    cmp r1, #0
    bne 20f
    movs r1, #1
    str r1, [sp, #16]
    ldr r1, [sp, #8]
    ldr r2, [r1, #24]
    movs r3, #33
    subs r3, r3, r2
    beq 20f                    /* no lock: all tiles drawn */
    mov lr, r3
    ldr r4, [r1, #4]
    ldr r2, [r1, #12]
    mov r11, r2
    ldr r2, [r1, #20]
    str r2, [sp, #20]
    movs r2, #0
    mvns r2, r2
    mov r12, r2                /* another row: no previous entry */
    b 1b
20: mov r0, r10
    add sp, #48
    pop {r4-r7}
    mov r8, r4
    mov r9, r5
    mov r10, r6
    mov r11, r7
    pop {r4-r7, pc}
    .pool
    .size gg_sms_bg, . - gg_sms_bg

#endif
