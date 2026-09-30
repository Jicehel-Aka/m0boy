/*
 * Thumb-1 (Cortex-M0+) versions of the memory and stack helpers that
 * generated code calls on every non-constant access and every CALL, RET,
 * PUSH and POP (gbl_rd, gbl_wr, gbl_push, gbl_pop; the C versions in
 * gbl_core.c stay the reference and are used elsewhere).
 * Enabled by GBL_CORE_ASM.
 *
 * The usual case, a stack in WRAM, touches no callee-saved register and
 * needs two literal loads: about 27 cycles instead of about 37 for the code
 * GCC produces (which saves four registers and loads five literals).
 */
#include "gbl_config.h"
#include "gbl_options.h"

#if GBL_CORE_ASM && defined(__ARM_ARCH_6M__)

    .syntax unified
    .thumb

/* gbl_cpu_t layout (gbl.h): sp is the halfword at offset 6. */
#define GBR_SP 6

/*
 * uint8_t gbl_rd(uint16_t a) / void gbl_wr(uint16_t a, uint8_t v)
 * Mapped page: one load or store through gbl_rmap / gbl_wmap, no stack.
 * Otherwise a tail call to gbl_rd_slow / gbl_wr_slow (arguments untouched).
 */
    .section .text.gbl_rd, "ax", %progbits
    .align 2
    .global gbl_rd
    .type gbl_rd, %function
    .thumb_func
gbl_rd:
    lsrs    r3, r0, #12
    lsls    r3, r3, #2
    ldr     r2, 9f
    ldr     r3, [r2, r3]
    cmp     r3, #0
    beq     1f
    ldrb    r0, [r3, r0]
    bx      lr
1:  /* HRAM (FF80-FFFE) without the C slow path: a + 0x80 is 0x1xxxx there. */
    movs    r3, #0x80
    adds    r3, r3, r0
    lsrs    r2, r3, #16
    beq     2f
    uxtb    r3, r3
    cmp     r3, #0x7F              /* FFFF is IE */
    beq     2f
    ldr     r2, 7f
    ldrb    r0, [r2, r3]
    bx      lr
2:
    ldr     r3, 8f
    bx      r3
    .align 2
7:  .word   gbl_hram
8:  .word   gbl_rd_slow
9:  .word   gbl_rmap
    .size gbl_rd, . - gbl_rd

    .section .text.gbl_wr, "ax", %progbits
    .align 2
    .global gbl_wr
    .type gbl_wr, %function
    .thumb_func
gbl_wr:
    lsrs    r3, r0, #12
    lsls    r3, r3, #2
    ldr     r2, 9f
    ldr     r3, [r2, r3]
    cmp     r3, #0
    beq     1f
    strb    r1, [r3, r0]
    bx      lr
1:  /* HRAM (FF80-FFFE) without the C slow path. */
    movs    r3, #0x80
    adds    r3, r3, r0
    lsrs    r2, r3, #16
    beq     2f
    uxtb    r3, r3
    cmp     r3, #0x7F              /* FFFF is IE */
    beq     2f
    ldr     r2, 7f
    strb    r1, [r2, r3]
    bx      lr
2:
    ldr     r3, 8f
    bx      r3
    .align 2
7:  .word   gbl_hram
8:  .word   gbl_wr_slow
9:  .word   gbl_wmap
    .size gbl_wr, . - gbl_wr

/*
 * void gbl_push(uint16_t v)
 * SP -= 2; stack in C000..DFFE: two byte stores into gbl_wram, otherwise
 * gbl_wr(SP + 1, v >> 8) then gbl_wr(SP, v).
 */
    .section .text.gbl_push, "ax", %progbits
    .align 2
    .global gbl_push
    .type gbl_push, %function
    .thumb_func
gbl_push:
    ldr     r2, 9f
    ldrh    r3, [r2, #GBR_SP]
    subs    r3, #2
    uxth    r3, r3
    strh    r3, [r2, #GBR_SP]
    movs    r1, #0x40
    lsls    r1, r1, #8             /* 0x4000 = -0xC000 mod 0x10000 */
    adds    r1, r3
    uxth    r1, r1                 /* offset in WRAM if SP >= 0xC000 */
    adds    r2, r1, #1
    lsrs    r2, r2, #13            /* offset + 1 < 0x2000: SP in C000..DFFE */
    bne     2f
    ldr     r2, 8f
    strb    r0, [r2, r1]
    lsrs    r0, r0, #8
    adds    r1, #1
    strb    r0, [r2, r1]
    bx      lr
2:
    push    {r4, r5, r6, lr}
    movs    r4, r3                 /* SP */
    movs    r5, r0                 /* v */
    adds    r0, r3, #1
    uxth    r0, r0
    lsls    r1, r5, #16
    lsrs    r1, r1, #24            /* v >> 8 */
    bl      gbl_wr
    movs    r0, r4
    uxtb    r1, r5
    bl      gbl_wr
    pop     {r4, r5, r6, pc}
    .align 2
8:  .word   gbl_wram
9:  .word   gbr
    .size gbl_push, . - gbl_push

/*
 * uint16_t gbl_pop(void)
 * Reads SP and SP + 1 (gbl_rd outside WRAM), then SP += 2.
 */
    .section .text.gbl_pop, "ax", %progbits
    .align 2
    .global gbl_pop
    .type gbl_pop, %function
    .thumb_func
gbl_pop:
    ldr     r2, 9f
    ldrh    r3, [r2, #GBR_SP]
    adds    r1, r3, #2
    strh    r1, [r2, #GBR_SP]
    movs    r1, #0x40
    lsls    r1, r1, #8
    adds    r1, r3
    uxth    r1, r1
    adds    r2, r1, #1
    lsrs    r2, r2, #13
    bne     2f
    ldr     r2, 8f
    ldrb    r0, [r2, r1]
    adds    r1, #1
    ldrb    r1, [r2, r1]
    lsls    r1, r1, #8
    orrs    r0, r1
    bx      lr
2:
    push    {r4, r5, r6, lr}
    movs    r4, r3
    movs    r0, r3
    bl      gbl_rd
    movs    r5, r0
    adds    r0, r4, #1
    uxth    r0, r0
    bl      gbl_rd
    lsls    r0, r0, #8
    orrs    r0, r5
    pop     {r4, r5, r6, pc}
    .align 2
8:  .word   gbl_wram
9:  .word   gbr
    .size gbl_pop, . - gbl_pop

#endif

#if GBL_UNPACK_ASM && defined(__ARM_ARCH_6M__)

    .syntax unified
    .thumb

/*
 * void gbl_unpack512_asm(const uint8_t *src, uint8_t *dst)
 *
 * LZSS decoder for one 512-byte ROM sector of a --meta-pack build (the C
 * reference is unpack512 in gbl_core.c).  Bits are kept left-aligned in r3,
 * so shifting out the token's flag bit leaves it in the carry: one LSLS
 * per token instead of shift, mask and test.  Refills a byte at a time to
 * keep at least 13 bits (the longest token).
 */
#if GBL_PPU_RAMFUNC
    .section .data.gbl_ramfunc.gbl_unpack512_asm, "ax", %progbits
#else
    .section .text.gbl_unpack512_asm, "ax", %progbits
#endif
    .align 2
    .global gbl_unpack512_asm
    .type gbl_unpack512_asm, %function
    .thumb_func
gbl_unpack512_asm:
    push    {r4-r7, lr}
    movs    r2, #2
    lsls    r2, r2, #8
    adds    r2, r2, r1             /* end of the sector */
    movs    r3, #0                 /* bits, next one in bit 31 */
    movs    r4, #0                 /* bits available */
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
2:  lsls    r3, r3, #1             /* C = flag: 1 literal, 0 reference */
    bcc     4f
    lsrs    r5, r3, #24
    strb    r5, [r1]
    adds    r1, #1
    lsls    r3, r3, #8
    subs    r4, #9
    cmp     r1, r2
    blo     1b
    pop     {r4-r7, pc}
4:  lsrs    r5, r3, #24            /* offset - 1 */
    lsls    r3, r3, #8
    lsrs    r6, r3, #28            /* length - 2 */
    lsls    r3, r3, #4
    subs    r4, #13
    mvns    r5, r5                 /* -offset */
    adds    r5, r5, r1             /* source: dst - offset */
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
    .size gbl_unpack512_asm, . - gbl_unpack512_asm

#endif
