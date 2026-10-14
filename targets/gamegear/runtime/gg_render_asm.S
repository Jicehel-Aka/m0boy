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
    .section .data.gg_ramfunc.gg_bg_tiles, "ax", %progbits
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
 * args: 0 names row, 4 locked names row, 8 tr, 12 locked tr, 16 first
 * locked tile, 20 tx0, 24 sh (4 * fine), 28 vram, 32 planes, 36 pairs (palette
 * 1 at +512), 40 spal (palette 1 at +64).
 * r8 previous fields, r9 previous palette, r10 slow groups, r11 group bit,
 * r12 args, lr tile.  Stack: [sp] fields and [sp, #4] palette of the last
 * group drawn here (palette 2: none, its pixels are not at out - 10), and
 * [sp, #8] name entry | tr << 16 of the previous tile.  A tile repeating the
 * previous one reuses its fields; a group equal to the last one copies its
 * five pixels. */
    .section .data.gg_ramfunc.gg_sms_bg, "ax", %progbits
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
    movs r5, #2
    mvns r6, r5
    push {r4-r6}               /* no group drawn, no previous tile */
    mov r12, r1
    movs r4, #0
    mov r8, r4
    mov r9, r4
    mov r10, r4
    mov lr, r4
    movs r4, #1
    mov r11, r4
10: mov r1, r12
    mov r2, lr
    ldr r3, [r1, #16]
    cmp r2, r3
    bhs 11f
    ldr r4, [r1, #0]
    ldr r5, [r1, #8]
    b 12f
11: ldr r4, [r1, #4]
    ldr r5, [r1, #12]
12: ldr r3, [r1, #20]
    adds r3, r3, r2
    movs r6, #31
    ands r3, r6
    lsls r3, r3, #1
    ldrh r3, [r4, r3]          /* name entry */
    lsls r6, r5, #16
    orrs r6, r3
    ldr r4, [sp, #8]
    cmp r4, r6
    bne 25f
    mov r7, r8                 /* the previous tile again: its fields */
    b 24f
25: str r6, [sp, #8]
    lsls r6, r3, #21           /* V flip */
    bpl 13f
    movs r6, #7
    subs r5, r6, r5
13: lsls r4, r3, #23
    lsrs r4, r4, #18           /* tile * 32 */
    lsls r5, r5, #2
    adds r4, r4, r5
    ldr r5, [r1, #28]
    adds r4, r4, r5            /* row bytes */
    ldr r5, [r1, #32]
    lsls r6, r3, #22           /* H flip: the other table */
    bpl 14f
    movs r6, #1
    lsls r6, r6, #10
    adds r5, r5, r6
14: ldrb r6, [r4, #0]
    lsls r6, r6, #2
    ldr r7, [r5, r6]
    ldrb r6, [r4, #1]
    lsls r6, r6, #2
    ldr r6, [r5, r6]
    lsls r6, r6, #1
    orrs r7, r6
    ldrb r6, [r4, #2]
    lsls r6, r6, #2
    ldr r6, [r5, r6]
    lsls r6, r6, #2
    orrs r7, r6
    ldrb r6, [r4, #3]
    lsls r6, r6, #2
    ldr r6, [r5, r6]
    lsls r6, r6, #3
    orrs r7, r6                /* fields of tile k */
24: lsls r3, r3, #20
    lsrs r3, r3, #31           /* its palette */
    cmp r2, #0
    beq 19f
    ldr r4, [r1, #24]          /* sh */
    mov r5, r8
    cmp r4, #0
    beq 15f
    mov r6, r9
    cmp r6, r3
    bne 17f                    /* two palettes: C path */
    lsrs r5, r4
    movs r6, #32
    subs r6, r6, r4
    mov r4, r7
    lsls r4, r6
    orrs r5, r4                /* group fields */
15: ldr r4, [sp, #0]
    cmp r4, r5
    bne 21f
    ldr r4, [sp, #4]
    cmp r4, r9
    beq 22f                    /* the last group again: copy it */
21: str r5, [sp, #0]
    mov r4, r9
    str r4, [sp, #4]
    ldr r4, [r1, #36]          /* pairs, spal of the group's palette */
    ldr r6, [r1, #40]
    mov r2, r9
    cmp r2, #0
    beq 16f
    movs r2, #1
    lsls r2, r2, #9
    adds r4, r4, r2
    adds r6, #64
16: lsls r1, r5, #24
    lsrs r1, r1, #23
    ldrh r1, [r4, r1]
    strh r1, [r0, #0]
    lsls r1, r5, #20
    lsrs r1, r1, #28
    lsls r1, r1, #1
    lsls r2, r5, #24
    lsrs r2, r2, #28
    lsls r2, r2, #5
    orrs r1, r2
    ldrh r1, [r4, r1]
    strh r1, [r0, #2]
    lsls r1, r5, #16
    lsrs r1, r1, #28
    lsls r1, r1, #2
    ldr r1, [r6, r1]
    lsls r2, r5, #12
    lsrs r2, r2, #28
    lsls r2, r2, #2
    ldr r2, [r6, r2]
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
    ldrh r1, [r4, r1]
    strh r1, [r0, #6]
    lsrs r1, r5, #28
    lsls r1, r1, #1
    lsls r2, r5, #4
    lsrs r2, r2, #28
    lsls r2, r2, #5
    orrs r1, r2
    ldrh r1, [r4, r1]
    strh r1, [r0, #8]
    b 18f
17: mov r4, r10
    mov r5, r11
    orrs r4, r5
    mov r10, r4
    movs r4, #2
    str r4, [sp, #4]           /* its pixels come later, from C */
18: adds r0, #10
    mov r4, r11
    lsls r4, r4, #1
    mov r11, r4
19: mov r8, r7
    mov r9, r3
    mov r2, lr
    adds r2, r2, #1
    mov lr, r2
    cmp r2, #33
    beq 20f
    b 10b
20: mov r0, r10
    add sp, #12
    pop {r4-r7}
    mov r8, r4
    mov r9, r5
    mov r10, r6
    mov r11, r7
    pop {r4-r7, pc}
22: mov r4, r0
    subs r4, #10
    ldrh r1, [r4, #0]
    strh r1, [r0, #0]
    ldrh r1, [r4, #2]
    strh r1, [r0, #2]
    ldrh r1, [r4, #4]
    strh r1, [r0, #4]
    ldrh r1, [r4, #6]
    strh r1, [r0, #6]
    ldrh r1, [r4, #8]
    strh r1, [r0, #8]
    b 18b
    .pool
    .size gg_sms_bg, . - gg_sms_bg

#endif
