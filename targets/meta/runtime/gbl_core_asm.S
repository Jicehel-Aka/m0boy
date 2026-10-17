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
#if GBL_ERAM_ON_SD
    /* Newest cartridge RAM sector (gbl_eram_rwin, GBL_ERAM_WIN_NONE never
     * matches): one load through gbl_eram_rptr, like the C gbl_rd. */
    ldr     r2, 13f
    ldr     r2, [r2]
    subs    r3, r0, r2
    lsrs    r3, r3, #9
    bne     1f
    ldr     r2, 14f
    ldr     r2, [r2]
    ldrb    r0, [r2, r0]
    bx      lr
1:
#endif
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
#if GBL_ERAM_ON_SD
13: .word   gbl_eram_rwin
14: .word   gbl_eram_rptr
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
#if GBL_ERAM_ON_SD
    /* Newest cartridge RAM sector, already dirty (gbl_eram_wwin): store a
     * changed byte and note the frame, like the C gbl_wr. */
    ldr     r2, 12f
    ldr     r2, [r2]
    subs    r3, r0, r2
    lsrs    r3, r3, #9
    bne     1f
    ldr     r2, 13f
    ldr     r2, [r2]
    ldrb    r3, [r2, r0]
    cmp     r3, r1
    beq     14f
    strb    r1, [r2, r0]
    ldr     r2, 15f
    ldr     r2, [r2]
    ldr     r3, 16f
    str     r2, [r3]
14:
    bx      lr
1:
#endif
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
#if GBL_ERAM_ON_SD
12: .word   gbl_eram_wwin
13: .word   gbl_eram_wptr
15: .word   gbl_frame
16: .word   gbl_save_last_change
#endif
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
 * The ALU helpers generated code calls: ADD, ADC, SUB, SBC, CP, INC, DEC
 * (gbl_cpu.c keeps the C versions for other targets).  Flags come from the
 * sign of a subtraction or bit 4/8 of a sum, with no branch and no stack
 * frame.  The M0 benchmark checks every input with -DBENCH_ALU_CHECK.
 */
.macro ALU_FN name
    .section .text.\name, "ax", %progbits
    .align 2
    .global \name
    .type \name, %function
    .thumb_func
\name:
.endm

/* half-carry of (r1 & 15) +/- (r0 & 15) [+/- r2] into r2 via \op, bit \bit */
    ALU_FN gbl_add
    ldr     r3, 9f
    ldrb    r1, [r3, #GBR_A]
    lsls    r2, r1, #28
    lsrs    r2, r2, #28
    adds    r1, r1, r0             /* a + v */
    lsls    r0, r0, #28
    lsrs    r0, r0, #28
    adds    r2, r2, r0
    lsrs    r2, r2, #4             /* half carry */
    strb    r2, [r3, #GBR_HF]
    lsrs    r2, r1, #8             /* carry */
    strb    r2, [r3, #GBR_CF]
    strb    r1, [r3, #GBR_A]
    strb    r1, [r3, #GBR_ZR]
    movs    r2, #0
    strb    r2, [r3, #GBR_NF]
    bx      lr
    .align 2
9:  .word   gbr
    .size gbl_add, . - gbl_add

    ALU_FN gbl_adc
    ldr     r3, 9f
    ldrb    r1, [r3, #GBR_A]
    ldrb    r2, [r3, #GBR_CF]
    push    {r4}
    lsls    r4, r1, #28
    lsrs    r4, r4, #28
    adds    r4, r4, r2
    adds    r1, r1, r2
    adds    r1, r1, r0             /* a + v + c */
    lsls    r0, r0, #28
    lsrs    r0, r0, #28
    adds    r4, r4, r0
    lsrs    r4, r4, #4
    strb    r4, [r3, #GBR_HF]
    pop     {r4}
    lsrs    r2, r1, #8
    strb    r2, [r3, #GBR_CF]
    strb    r1, [r3, #GBR_A]
    strb    r1, [r3, #GBR_ZR]
    movs    r2, #0
    strb    r2, [r3, #GBR_NF]
    bx      lr
    .align 2
9:  .word   gbr
    .size gbl_adc, . - gbl_adc

    ALU_FN gbl_sub
    ldr     r3, 9f
    ldrb    r1, [r3, #GBR_A]
    lsls    r2, r1, #28
    lsrs    r2, r2, #28
    subs    r1, r1, r0             /* a - v */
    lsls    r0, r0, #28
    lsrs    r0, r0, #28
    subs    r2, r2, r0
    lsrs    r2, r2, #31            /* half borrow */
    strb    r2, [r3, #GBR_HF]
    lsrs    r2, r1, #31            /* borrow */
    strb    r2, [r3, #GBR_CF]
    strb    r1, [r3, #GBR_A]
    strb    r1, [r3, #GBR_ZR]
    movs    r2, #1
    strb    r2, [r3, #GBR_NF]
    bx      lr
    .align 2
9:  .word   gbr
    .size gbl_sub, . - gbl_sub

    ALU_FN gbl_sbc
    ldr     r3, 9f
    ldrb    r1, [r3, #GBR_A]
    ldrb    r2, [r3, #GBR_CF]
    push    {r4}
    lsls    r4, r1, #28
    lsrs    r4, r4, #28
    subs    r4, r4, r2
    subs    r1, r1, r2
    subs    r1, r1, r0             /* a - v - c */
    lsls    r0, r0, #28
    lsrs    r0, r0, #28
    subs    r4, r4, r0
    lsrs    r4, r4, #31
    strb    r4, [r3, #GBR_HF]
    pop     {r4}
    lsrs    r2, r1, #31
    strb    r2, [r3, #GBR_CF]
    strb    r1, [r3, #GBR_A]
    strb    r1, [r3, #GBR_ZR]
    movs    r2, #1
    strb    r2, [r3, #GBR_NF]
    bx      lr
    .align 2
9:  .word   gbr
    .size gbl_sbc, . - gbl_sbc

    ALU_FN gbl_cp
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

    ALU_FN gbl_inc
    ldr     r3, 9f
    lsls    r2, r0, #28
    lsrs    r2, r2, #28
    adds    r2, #1
    lsrs    r2, r2, #4             /* (v & 15) == 15 */
    strb    r2, [r3, #GBR_HF]
    movs    r2, #0
    strb    r2, [r3, #GBR_NF]
    adds    r0, #1
    uxtb    r0, r0
    strb    r0, [r3, #GBR_ZR]
    bx      lr
    .align 2
9:  .word   gbr
    .size gbl_inc, . - gbl_inc

    ALU_FN gbl_dec
    ldr     r3, 9f
    lsls    r2, r0, #28
    lsrs    r2, r2, #28
    subs    r2, #1
    lsrs    r2, r2, #31            /* (v & 15) == 0 */
    strb    r2, [r3, #GBR_HF]
    movs    r2, #1
    strb    r2, [r3, #GBR_NF]
    subs    r0, #1
    uxtb    r0, r0
    strb    r0, [r3, #GBR_ZR]
    bx      lr
    .align 2
9:  .word   gbr
    .size gbl_dec, . - gbl_dec

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
#define M0_ROM_RAMFUNC GBL_PPU_RAMFUNC
#include "m0_rom_lzss.inc"
#endif
