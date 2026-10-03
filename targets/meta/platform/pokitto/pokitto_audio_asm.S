/*
 * Pokitto audio interrupt (CT32B0 match 1, 22,050 a second) in Thumb-1 from
 * SRAM: r0-r3 only (the core stacks them on entry: no push), one GPIO base
 * register, literals in SRAM, no flash access.  Same output as the C
 * CT32B0_IRQHandler of pokitto_audio.c (POKITTO_AUDIO_ASM=0):
 *
 *   SET[1] = v << 28              DAC bits 0-3 on P1_28..31
 *   CLR[1] = ~v << 28
 *   SET[2] = (v >> 4 & 7) << 20   DAC bits 4-6 on P2_20..22
 *   CLR[2] = (~v >> 4 & 7) << 20
 *   SET[2] or CLR[2] = 1 << 23    DAC bit 7 on P2_23, last (the emulator in
 *                                 tools/meta-emu-sdl latches a sample on a
 *                                 write to it); CLR is SET + 0x80, so the
 *                                 choice is an offset, (~v & 0x80).
 *
 * The queue is pokitto_audio_q: uint16 tail, uint16 head, 1024 samples.
 */
#ifndef POKITTO_AUDIO_ASM
#define POKITTO_AUDIO_ASM 1
#endif
#if POKITTO_AUDIO_ASM
    .syntax unified
    .cpu cortex-m0plus
    .thumb

    .section .data.gbl_ramfunc.CT32B0_IRQHandler, "ax", %progbits
    .align 2
    .global CT32B0_IRQHandler
    .type CT32B0_IRQHandler, %function
    .thumb_func
CT32B0_IRQHandler:
    ldr r0, .Lq
    ldr r1, .Lir
    movs r2, #2
    str r2, [r1]            /* CT32B0 IR: match 1 handled */
    ldrh r1, [r0]           /* tail */
    ldrh r2, [r0, #2]       /* head */
    cmp r1, r2
    beq 1f                  /* empty: the DAC keeps its value */
    adds r2, r0, r1
    ldrb r2, [r2, #4]       /* v */
    adds r1, #1
    lsls r1, r1, #22
    lsrs r1, r1, #22
    strh r1, [r0]           /* tail + 1 mod 1024 */
    ldr r0, .Lset1
    movs r3, #0x80          /* CLR - SET */
    lsls r1, r2, #28
    str r1, [r0]            /* SET[1] */
    mvns r1, r2
    lsls r1, r1, #28
    str r1, [r0, r3]        /* CLR[1] */
    adds r0, #4             /* SET[2] */
    lsls r1, r2, #25
    lsrs r1, r1, #29
    lsls r1, r1, #20
    str r1, [r0]            /* SET[2] bits 20-22 */
    mvns r2, r2
    lsls r1, r2, #25
    lsrs r1, r1, #29
    lsls r1, r1, #20
    str r1, [r0, r3]        /* CLR[2] bits 20-22 */
    ands r2, r3             /* ~v & 0x80: 0 SET, 0x80 CLR */
    movs r1, #1
    lsls r1, r1, #23
    str r1, [r0, r2]        /* bit 23 last */
1:  bx lr
    .align 2
.Lq:    .word pokitto_audio_q
.Lir:   .word 0x40014000
.Lset1: .word 0xA0002204
    .size CT32B0_IRQHandler, . - CT32B0_IRQHandler
#endif
