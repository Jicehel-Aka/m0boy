/* Shared Thumb-1 8/4 LZSS decoder, formerly gbl_core_asm.S. */
    .syntax unified
    .thumb
#ifndef M0_ROM_RAMFUNC
#define M0_ROM_RAMFUNC 1
#endif
#if M0_ROM_RAMFUNC
    .section .data.m0_rom_unpack512_asm, "ax", %progbits
#else
    .section .text.m0_rom_unpack512_asm, "ax", %progbits
#endif
    .align 2
    .global m0_rom_unpack512_asm
    .global gbl_unpack512_asm /* backwards-compatible GB symbol */
    .type m0_rom_unpack512_asm, %function
    .type gbl_unpack512_asm, %function
    .thumb_func
m0_rom_unpack512_asm:
    .thumb_func
gbl_unpack512_asm:
    push    {r4-r7, lr}
    movs    r2, #2
    lsls    r2, r2, #8
    adds    r2, r2, r1
    movs    r3, #0
    movs    r4, #0
1:  cmp     r4, #13
    bge     2f
    ldrb    r5, [r0]
    adds    r0, #1
    movs    r6, #24
    subs    r6, r6, r4
    lsls    r5, r5, r6
    orrs    r3, r5
    adds    r4, #8
    b       1b
2:  lsls    r3, r3, #1
    bcc     4f
    lsrs    r5, r3, #24
    strb    r5, [r1]
    adds    r1, #1
    lsls    r3, r3, #8
    subs    r4, #9
    cmp     r1, r2
    blo     1b
    pop     {r4-r7, pc}
4:  lsrs    r5, r3, #24
    lsls    r3, r3, #8
    lsrs    r6, r3, #28
    lsls    r3, r3, #4
    subs    r4, #13
    mvns    r5, r5
    adds    r5, r5, r1
    adds    r6, #2
5:  ldrb    r7, [r5]
    adds    r5, #1
    strb    r7, [r1]
    adds    r1, #1
    cmp     r1, r2
    bhs     6f
    subs    r6, #1
    bne     5b
    b       1b
6:  pop     {r4-r7, pc}
    .size m0_rom_unpack512_asm, . - m0_rom_unpack512_asm
    .size gbl_unpack512_asm, . - gbl_unpack512_asm
