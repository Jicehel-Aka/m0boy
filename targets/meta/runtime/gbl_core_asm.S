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
