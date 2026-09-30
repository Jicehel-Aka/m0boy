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

/* gbl_cpu_t layout (gbl.h): sp is the halfword at offset 6, then pc, and
 * the bytes a, zr, cf, hf, nf. */
#define GBR_SP 6
#define GBR_A 10
#define GBR_ZR 11
#define GBR_CF 12
#define GBR_HF 13
#define GBR_NF 14
#define GBR_ROM_BANK 16

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
1:
#if GBL_ROM_STREAM
    /* Streamed ROM chunk (4000-7FFF): the byte of the last sector used,
     * like stream_read's first test, without the C call. */
    lsrs    r2, r0, #14
    cmp     r2, #1
    bne     1f
    mov     ip, r0
    ldr     r2, 4f
    ldrh    r2, [r2, #GBR_ROM_BANK]
    subs    r2, #1
    lsls    r2, r2, #14
    adds    r2, r2, r0             /* ROM offset */
    ldr     r3, 5f
    ldrb    r3, [r3]               /* last slot */
    ldr     r1, 6f
    lsls    r0, r3, #2
    ldr     r1, [r1, r0]           /* its sector */
    lsrs    r0, r2, #9
    cmp     r0, r1
    bne     3f
    lsls    r3, r3, #9
    ldr     r1, 10f
    adds    r1, r1, r3
    lsls    r2, r2, #23
    lsrs    r2, r2, #23
    ldrb    r0, [r1, r2]
    bx      lr
3:
    mov     r0, ip
    b       2f
#endif
1:  /* HRAM (FF80-FFFE) without the C slow path: a + 0x80 is 0x1xxxx there;
     * IO (FF00-FF7F) straight to gbl_io_rd. */
    movs    r3, #0x80
    adds    r3, r3, r0
    lsrs    r2, r3, #16
    beq     11f
    uxtb    r3, r3
    cmp     r3, #0x7F              /* FFFF is IE */
    beq     2f
    ldr     r2, 7f
    ldrb    r0, [r2, r3]
    bx      lr
11:
    lsrs    r2, r0, #8
    cmp     r2, #0xFF
    bne     2f
    uxtb    r0, r0
    ldr     r3, 12f
    bx      r3
2:
    ldr     r3, 8f
    bx      r3
    .align 2
7:  .word   gbl_hram
8:  .word   gbl_rd_slow
9:  .word   gbl_rmap
12: .word   gbl_io_rd
#if GBL_ROM_STREAM
4:  .word   gbr
5:  .word   gbl_stream_last
6:  .word   gbl_stream_tag
10: .word   gbl_stream_buf
#endif
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
1:  /* MBC registers and VRAM straight to their handlers (gbl_wr_slow would
     * test six ranges first; games switch banks around every banked call). */
    lsrs    r2, r0, #15
    beq     3f                     /* 0000-7FFF */
    lsrs    r2, r0, #13
    cmp     r2, #4
    beq     4f                     /* 8000-9FFF */
    /* HRAM (FF80-FFFE) without the C slow path, IO (FF00-FF7F) straight
     * to gbl_io_wr. */
    movs    r3, #0x80
    adds    r3, r3, r0
    lsrs    r2, r3, #16
    beq     10f
    uxtb    r3, r3
    cmp     r3, #0x7F              /* FFFF is IE */
    beq     2f
    ldr     r2, 7f
    strb    r1, [r2, r3]
    bx      lr
10:
    lsrs    r2, r0, #8
    cmp     r2, #0xFF
    bne     2f
    uxtb    r0, r0
    ldr     r3, 11f
    bx      r3
2:
    ldr     r3, 8f
    bx      r3
3:
    ldr     r3, 5f
    bx      r3
4:
    lsls    r0, r0, #19
    lsrs    r0, r0, #19            /* VRAM offset */
    ldr     r3, 6f
    bx      r3
    .align 2
5:  .word   gbl_mbc_write
6:  .word   gbl_vram_wr
11: .word   gbl_io_wr
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

/*
 * void gbl_cp(uint8_t v)
 * CP: zr = A - v, cf = A < v, hf = (A & 15) < (v & 15), nf = 1, from the
 * borrows of two subtractions (SBCS/NEGS), with no stack frame.  The M0
 * benchmark checks every (A, v) pair with -DBENCH_ALU_CHECK.
 */
    .section .text.gbl_cp, "ax", %progbits
    .align 2
    .global gbl_cp
    .type gbl_cp, %function
    .thumb_func
gbl_cp:
    ldr     r3, 9f
    ldrb    r1, [r3, #GBR_A]
    subs    r2, r1, r0
    strb    r2, [r3, #GBR_ZR]
    sbcs    r2, r2                 /* -1 on borrow */
    negs    r2, r2
    strb    r2, [r3, #GBR_CF]
    lsls    r1, r1, #28
    lsls    r0, r0, #28
    subs    r1, r1, r0             /* low nibbles */
    sbcs    r2, r2
    negs    r2, r2
    strb    r2, [r3, #GBR_HF]
    movs    r2, #1
    strb    r2, [r3, #GBR_NF]
    bx      lr
    .align 2
9:  .word   gbr
    .size gbl_cp, . - gbl_cp

/*
 * uint32_t gbl_oam_dma_asm(uint8_t *oam, const uint8_t *src)
 * OAM DMA from a mapped page: copies 160 bytes into the word-aligned OAM
 * and returns the OR of old ^ new (nonzero when anything changed).  A
 * word-aligned source goes 16 bytes per iteration with LDM/STM (about 28
 * cycles); otherwise 4 bytes per iteration.
 */
    .section .text.gbl_oam_dma_asm, "ax", %progbits
    .align 2
    .global gbl_oam_dma_asm
    .type gbl_oam_dma_asm, %function
    .thumb_func
gbl_oam_dma_asm:
    push    {r4-r7}
    movs    r3, #0                 /* old ^ new, accumulated */
    movs    r2, #3
    tst     r1, r2
    bne     2f
    movs    r2, #10                /* 10 x 16 bytes */
1:
    ldm     r1!, {r4, r5}
    ldr     r6, [r0]
    ldr     r7, [r0, #4]
    eors    r6, r4
    eors    r7, r5
    orrs    r3, r6
    orrs    r3, r7
    stm     r0!, {r4, r5}
    ldm     r1!, {r4, r5}
    ldr     r6, [r0]
    ldr     r7, [r0, #4]
    eors    r6, r4
    eors    r7, r5
    orrs    r3, r6
    orrs    r3, r7
    stm     r0!, {r4, r5}
    subs    r2, #1
    bne     1b
    b       4f
2:
    movs    r2, #40                /* 40 x 4 bytes */
3:
    ldrb    r4, [r1]
    ldrb    r5, [r0]
    eors    r5, r4
    orrs    r3, r5
    strb    r4, [r0]
    ldrb    r4, [r1, #1]
    ldrb    r5, [r0, #1]
    eors    r5, r4
    orrs    r3, r5
    strb    r4, [r0, #1]
    ldrb    r4, [r1, #2]
    ldrb    r5, [r0, #2]
    eors    r5, r4
    orrs    r3, r5
    strb    r4, [r0, #2]
    ldrb    r4, [r1, #3]
    ldrb    r5, [r0, #3]
    eors    r5, r4
    orrs    r3, r5
    strb    r4, [r0, #3]
    adds    r0, #4
    adds    r1, #4
    subs    r2, #1
    bne     3b
4:
    movs    r0, r3
    pop     {r4-r7}
    bx      lr
    .size gbl_oam_dma_asm, . - gbl_oam_dma_asm

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
