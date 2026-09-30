/*
 * Thumb-1 (Cortex-M0+) interpreter loop: gbl_interp_block of gbl_cpu.c,
 * enabled by GBL_INTERP_ASM.  The loop (event check, fetch, cycles,
 * dispatch through a 256-entry table of halfword offsets) and the common opcodes
 * are here (the CB prefix too); the others (DAA, EI, RETI, HALT,
 * STOP, PUSH/POP AF, SP arithmetic, LD (nn),SP, undefined opcodes, and
 * the DEC A / JR NZ,-3 delay-loop shortcut) go through the C
 * gbl_interp_op, which runs one instruction exactly as the C loop does.
 * GCC's loop spent about 120 cycles per instruction before executing it
 * (literal reloads around calls, a switch helper).
 *
 * Checked on the M0 benchmark against the C interpreter with the state
 * and pixel hashes, in the usual mode and with BENCH_INTERP_ONLY (every
 * instruction of the game interpreted).
 */
#include "gbl_config.h"
#include "gbl_options.h"

#if GBL_INTERP_ASM && defined(__ARM_ARCH_6M__)

    .syntax unified
    .thumb

/* gbl_cpu_t (gbl.h; offsets checked in gbl_cpu.c) */
#define R_BC 0
#define R_DE 2
#define R_HL 4
#define R_SP 6
#define R_PC 8
#define R_A 10
#define R_ZR 11
#define R_CF 12
#define R_HF 13
#define R_NF 14
#define R_IME 15
#define R_CYC 20
#define R_NEXT 24

/* r4 = gbr, r5 = instructions in this block, r6 = n16 (operands),
 * r7 = opcode, r8 = register offsets, r9 = EI delay, r10 = gbl_rmap,
 * r11 = dispatch table.  r0-r3 are scratch and do not survive calls. */

/* r2 = register (op field in r0: B C D E H L (HL) A) */
.macro GET_R
    cmp     r0, #6
    beq     81f
    mov     r1, r8
    ldrb    r0, [r1, r0]
    ldrb    r2, [r4, r0]
    b       82f
81:
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    mov     r2, r0
82:
.endm

/* register (op field in r0) = r2 */
.macro SET_R
    cmp     r0, #6
    beq     71f
    mov     r1, r8
    ldrb    r0, [r1, r0]
    strb    r2, [r4, r0]
    b       72f
71:
    mov     r1, r2
    ldrh    r0, [r4, #R_HL]
    bl      gbl_wr
72:
.endm

/* gbr.pc += n */
.macro PC_ADD n
    ldrh    r0, [r4, #R_PC]
    adds    r0, #\n
    strh    r0, [r4, #R_PC]
.endm

/* branch to \yes when the condition of op bits 3-4 (NZ, Z, NC, C) holds */
.macro COND yes
    lsrs    r1, r7, #3
    movs    r0, #3
    ands    r1, r0
    cmp     r1, #2
    bhs     91f
    ldrb    r0, [r4, #R_ZR]        /* NZ: zr != 0, Z: zr == 0 */
    b       92f
91:
    ldrb    r0, [r4, #R_CF]        /* NC: cf == 0, C: cf != 0 */
    subs    r1, #2
    movs    r2, #1
    eors    r1, r2                 /* NC -> 1, C -> 0: like NZ -> 0, Z -> 1 */
92:
    cmp     r0, #0
    beq     93f
    cmp     r1, #0                 /* flag set: NZ (or C) holds */
    beq     \yes
    b       94f
93:
    cmp     r1, #0                 /* flag clear: Z (or NC) holds */
    bne     \yes
94:
.endm

/* cycles += n */
.macro CYC_ADD n
    ldr     r0, [r4, #R_CYC]
    adds    r0, #\n
    str     r0, [r4, #R_CYC]
.endm

    .section .text.gbl_interp_block, "ax", %progbits
    .align 2
    .global gbl_interp_block
    .type gbl_interp_block, %function
    .thumb_func
gbl_interp_block:
    push    {r4-r7, lr}
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    push    {r4-r7}
    ldr     r4, .Lgbr
    ldr     r0, .Lregofs
    mov     r8, r0
    ldr     r0, .Lrmap
    mov     r10, r0
    ldr     r0, .Ltable
    mov     r11, r0
    movs    r5, #0
    movs    r0, #0
    mov     r9, r0
.Ltop:
    mov     r0, r9                 /* EI takes effect after the next instruction */
    cmp     r0, #0
    bne     .Lno_event
    ldr     r0, [r4, #R_CYC]
    ldr     r1, [r4, #R_NEXT]
    subs    r0, r0, r1
    bmi     .Lfetch
    bl      gbl_sync
    bl      gbl_irq_enter
    cmp     r0, #0
    beq     .Lfetch
    b       .Lret
.Lno_event:
    movs    r0, #0
    mov     r9, r0
.Lfetch:
    ldrh    r6, [r4, #R_PC]
    lsrs    r1, r6, #12
    lsls    r1, r1, #2
    mov     r0, r10
    ldr     r1, [r0, r1]
    cmp     r1, #0
    beq     .Lslow_fetch
    lsls    r2, r6, #20            /* opcode and operands in one page? */
    lsrs    r2, r2, #20
    adds    r2, #2
    lsrs    r2, r2, #12
    bne     .Lslow_fetch
    adds    r1, r1, r6
    ldrb    r7, [r1]
    ldrb    r2, [r1, #1]
    ldrb    r3, [r1, #2]
    lsls    r3, r3, #8
    orrs    r3, r2
.Lfetched:
    adds    r6, #1
    strh    r6, [r4, #R_PC]
    mov     r6, r3
    ldr     r0, .Lcyc
    ldrb    r0, [r0, r7]
    ldr     r1, [r4, #R_CYC]
    adds    r1, r1, r0
    str     r1, [r4, #R_CYC]
    lsls    r0, r7, #1
    mov     r1, r11
    ldrsh   r0, [r1, r0]           /* handlers precede the table */
    lsls    r0, r0, #1
    adds    r0, r0, r1
    mov     pc, r0

.Lslow_fetch:  /* same reads as the C loop, in the same order */
    mov     r0, r6
    bl      gbl_rd
    mov     r7, r0
    adds    r0, r6, #1
    uxth    r0, r0
    bl      gbl_rd
    push    {r0}
    adds    r0, r6, #2
    uxth    r0, r0
    bl      gbl_rd
    lsls    r3, r0, #8
    pop     {r0}
    orrs    r3, r0
    b       .Lfetched

.Lnext:
    adds    r5, #1
    cmp     r5, #64
    blo     .Ltop
    mov     r0, r9
    cmp     r0, #0
    bne     .Ltop
    b       .Lret
.Ljump:  /* after a control transfer */
    adds    r5, #1
    ldr     r0, .Linstrs
    ldr     r1, [r0]
    adds    r1, r1, r5
    str     r1, [r0]
    movs    r5, #0
    /* Start a new block here when the dispatcher would call this function
     * again: gbl_exec's frame not left (its SP bound), and no compiled code
     * at the target.  Same block boundaries, without the return, the
     * dispatcher loop and the call. */
    ldr     r0, .Lbounded
    ldrb    r0, [r0]
    cmp     r0, #0
    beq     1f
    ldr     r0, .Lsp0
    ldrh    r0, [r0]
    ldrh    r1, [r4, #R_SP]
    subs    r1, r1, r0
    sxth    r1, r1
    cmp     r1, #0
    bgt     .Lret
1:
    ldr     r0, .Linterp_only
    ldrb    r0, [r0]
    cmp     r0, #0
    bne     2f
    ldrh    r0, [r4, #R_PC]
    bl      gbl_lookup
    cmp     r0, #0
    bne     .Lret
2:
    b       .Ltop
.Lret:  /* gbl_interp_instrs += instructions of the block (counted per block:
         * a reader in gbl_platform_vblank may see up to 63 of them late) */
    ldr     r0, .Linstrs
    ldr     r1, [r0]
    adds    r1, r1, r5
    str     r1, [r0]
    pop     {r4-r7}
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    pop     {r4-r7, pc}

    .align 2
.Lgbr:      .word gbr
.Lregofs:   .word .Lreg_offsets
.Lrmap:     .word gbl_rmap
.Ltable:    .word .Lt
.Lcyc:      .word gbl_cyc_main
.Linstrs:   .word gbl_interp_instrs
.Lbounded:  .word gbl_exec_bounded
.Lsp0:      .word gbl_exec_sp0
.Linterp_only: .word gbl_interp_only
.Lalu_fn:   .word gbl_add
            .word gbl_adc
            .word gbl_sub
            .word gbl_sbc

/* ---- handlers (offsets from the dispatch table at the end) ---- */

H_C:  /* the other opcodes: C, one instruction */
    mov     r0, r7
    mov     r1, r6
    bl      gbl_interp_op
    lsrs    r1, r0, #1             /* EI */
    mov     r9, r1
    lsls    r0, r0, #31
    beq     .Lnext_far
    b       .Ljump
.Lnext_far:
    b       .Lnext

H_NOP:
    b       .Lnext

H_LDRR:  /* LD r, r' */
    movs    r0, #7
    ands    r0, r7
    GET_R
    lsrs    r0, r7, #3
    movs    r1, #7
    ands    r0, r1
    SET_R
    b       .Lnext

H_ALUN:  /* ALU A, n */
    uxtb    r2, r6
    PC_ADD  1
    b       .Lalu
H_ALUR:  /* ALU A, r */
    movs    r0, #7
    ands    r0, r7
    GET_R
.Lalu:  /* r2 = operand; operation in op bits 3-5 */
    lsrs    r1, r7, #3
    movs    r0, #7
    ands    r1, r0
    cmp     r1, #4
    blo     5f
    beq     6f
    cmp     r1, #7
    beq     7f
    ldrb    r0, [r4, #R_A]         /* XOR, OR */
    cmp     r1, #5
    bne     8f
    eors    r0, r2
    b       9f
8:
    orrs    r0, r2
9:
    strb    r0, [r4, #R_A]
    strb    r0, [r4, #R_ZR]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_HF]
    strb    r0, [r4, #R_CF]
    b       .Lnext
6:  /* AND */
    ldrb    r0, [r4, #R_A]
    ands    r0, r2
    strb    r0, [r4, #R_A]
    strb    r0, [r4, #R_ZR]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_CF]
    movs    r0, #1
    strb    r0, [r4, #R_HF]
    b       .Lnext
5:  /* ADD ADC SUB SBC: C helpers */
    lsls    r1, r1, #2
    ldr     r0, .Lalu_tab
    ldr     r1, [r0, r1]
    mov     r0, r2
    blx     r1
    b       .Lnext
7:  /* CP */
    mov     r0, r2
    bl      gbl_cp
    b       .Lnext
    .align 2
.Lalu_tab:  .word .Lalu_fn

H_INC:
    lsrs    r0, r7, #3
    GET_R
    movs    r0, #0
    lsls    r1, r2, #28
    lsrs    r1, r1, #28
    cmp     r1, #15
    bne     1f
    movs    r0, #1
1:
    strb    r0, [r4, #R_HF]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    adds    r2, #1
    uxtb    r2, r2
    strb    r2, [r4, #R_ZR]
    lsrs    r0, r7, #3
    SET_R
    b       .Lnext

H_DEC:
    lsrs    r0, r7, #3
    GET_R
    movs    r0, #0
    lsls    r1, r2, #28
    bne     1f
    movs    r0, #1
1:
    strb    r0, [r4, #R_HF]
    movs    r0, #1
    strb    r0, [r4, #R_NF]
    subs    r2, #1
    uxtb    r2, r2
    strb    r2, [r4, #R_ZR]
    lsrs    r0, r7, #3
    SET_R
    b       .Lnext

H_LDRN:
    uxtb    r2, r6
    lsrs    r0, r7, #3
    SET_R
    PC_ADD  1
    b       .Lnext

H_LDRRNN:
    lsrs    r0, r7, #4
    lsls    r0, r0, #1
    strh    r6, [r4, r0]
    PC_ADD  2
    b       .Lnext

H_INCRR:
    lsrs    r0, r7, #4
    lsls    r0, r0, #1
    ldrh    r1, [r4, r0]
    adds    r1, #1
    strh    r1, [r4, r0]
    b       .Lnext

H_DECRR:
    lsrs    r0, r7, #4
    lsls    r0, r0, #1
    ldrh    r1, [r4, r0]
    subs    r1, #1
    strh    r1, [r4, r0]
    b       .Lnext

H_ADDHL:
    lsrs    r0, r7, #4
    lsls    r0, r0, #1
    ldrh    r0, [r4, r0]
    bl      gbl_addhl
    b       .Lnext

H_ST_BC:
    ldrh    r0, [r4, #R_BC]
    b       1f
H_ST_DE:
    ldrh    r0, [r4, #R_DE]
1:
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    b       .Lnext

H_LD_BC:
    ldrh    r0, [r4, #R_BC]
    b       1f
H_LD_DE:
    ldrh    r0, [r4, #R_DE]
1:
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    b       .Lnext

H_LDI_ST:
    ldrh    r0, [r4, #R_HL]
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    ldrh    r0, [r4, #R_HL]
    adds    r0, #1
    strh    r0, [r4, #R_HL]
    b       .Lnext

H_LDD_ST:
    ldrh    r0, [r4, #R_HL]
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    ldrh    r0, [r4, #R_HL]
    subs    r0, #1
    strh    r0, [r4, #R_HL]
    b       .Lnext

H_LDI_LD:
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    ldrh    r0, [r4, #R_HL]
    adds    r0, #1
    strh    r0, [r4, #R_HL]
    b       .Lnext

H_LDD_LD:
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    ldrh    r0, [r4, #R_HL]
    subs    r0, #1
    strh    r0, [r4, #R_HL]
    b       .Lnext

H_LDH_ST:  /* LDH (n), A */
    uxtb    r0, r6
    movs    r1, #0xFF
    lsls    r1, r1, #8
    orrs    r0, r1
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    PC_ADD  1
    b       .Lnext

H_LDH_LD:  /* LDH A, (n) */
    uxtb    r0, r6
    movs    r1, #0xFF
    lsls    r1, r1, #8
    orrs    r0, r1
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    PC_ADD  1
    b       .Lnext

H_LDC_ST:  /* LD (C), A */
    ldrb    r0, [r4, #0]
    movs    r1, #0xFF
    lsls    r1, r1, #8
    orrs    r0, r1
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    b       .Lnext

H_LDC_LD:  /* LD A, (C) */
    ldrb    r0, [r4, #0]
    movs    r1, #0xFF
    lsls    r1, r1, #8
    orrs    r0, r1
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    b       .Lnext

H_ST_NN:
    mov     r0, r6
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    PC_ADD  2
    b       .Lnext

H_LD_NN:
    mov     r0, r6
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    PC_ADD  2
    b       .Lnext

H_JR:
    ldrh    r0, [r4, #R_PC]
    sxtb    r1, r6
    adds    r0, #1
    adds    r0, r0, r1
    strh    r0, [r4, #R_PC]
    b       .Ljump

H_JRCC:
#if GBL_INTERP_DELAY_LOOP
    cmp     r7, #0x20              /* JR NZ,-3: the C shortcut of DEC A loops */
    bne     1f
    uxtb    r0, r6
    cmp     r0, #0xFD
    bne     1f
    b       H_C
1:
#endif
    PC_ADD  1
    COND    2f
    b       .Lnext
2:
    ldrh    r0, [r4, #R_PC]
    sxtb    r1, r6
    adds    r0, r0, r1
    strh    r0, [r4, #R_PC]
    CYC_ADD 4
    b       .Ljump

H_JP:
    strh    r6, [r4, #R_PC]
    b       .Ljump

H_JPCC:
    PC_ADD  2
    COND    1f
    b       .Lnext
1:
    strh    r6, [r4, #R_PC]
    CYC_ADD 4
    b       .Ljump

H_CALL:
    PC_ADD  2
    bl      gbl_push
    strh    r6, [r4, #R_PC]
    b       .Ljump

H_CALLCC:
    PC_ADD  2
    COND    1f
    b       .Lnext
1:
    ldrh    r0, [r4, #R_PC]
    bl      gbl_push
    strh    r6, [r4, #R_PC]
    CYC_ADD 12
    b       .Ljump

H_RET:
    bl      gbl_pop
    strh    r0, [r4, #R_PC]
    b       .Ljump

H_RETCC:
    COND    1f
    b       .Lnext
1:
    bl      gbl_pop
    strh    r0, [r4, #R_PC]
    CYC_ADD 12
    b       .Ljump

H_RST:
    ldrh    r0, [r4, #R_PC]
    bl      gbl_push
    movs    r0, #0x38
    ands    r0, r7
    strh    r0, [r4, #R_PC]
    b       .Ljump

H_JPHL:
    ldrh    r0, [r4, #R_HL]
    strh    r0, [r4, #R_PC]
    b       .Ljump

H_PUSH:
    lsrs    r0, r7, #4
    movs    r1, #3
    ands    r0, r1
    lsls    r0, r0, #1
    ldrh    r0, [r4, r0]
    bl      gbl_push
    b       .Lnext

H_POP:
    bl      gbl_pop
    lsrs    r1, r7, #4
    movs    r2, #3
    ands    r1, r2
    lsls    r1, r1, #1
    strh    r0, [r4, r1]
    b       .Lnext

H_CB:  /* CB prefix: BIT, RES, SET here, rotates/shifts/SWAP via gbl_cbop */
    PC_ADD  1
    uxtb    r6, r6                 /* CB opcode */
    movs    r0, #7
    ands    r0, r6
    movs    r1, #4                 /* extra cycles: 4, (HL) 12, BIT n,(HL) 8 */
    cmp     r0, #6
    bne     1f
    movs    r1, #12
    lsrs    r2, r6, #6
    cmp     r2, #1
    bne     1f
    movs    r1, #8
1:
    ldr     r2, [r4, #R_CYC]
    adds    r2, r2, r1
    str     r2, [r4, #R_CYC]
    GET_R                          /* r0 = register field: r2 = value */
    lsrs    r1, r6, #6
    beq     5f
    lsrs    r3, r6, #3
    movs    r0, #7
    ands    r3, r0
    movs    r0, #1
    lsls    r0, r3                 /* bit mask */
    cmp     r1, #2
    blo     2f
    beq     3f
    orrs    r2, r0                 /* SET */
    b       6f
3:
    bics    r2, r0                 /* RES */
    b       6f
2:
    ands    r0, r2                 /* BIT: Z from the bit, N 0, H 1 */
    strb    r0, [r4, #R_ZR]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    movs    r0, #1
    strb    r0, [r4, #R_HF]
    b       .Lnext
5:
    mov     r1, r2
    mov     r0, r6
    bl      gbl_cbop
    mov     r2, r0
6:
    movs    r0, #7
    ands    r0, r6
    SET_R
    b       .Lnext

H_ROTA:  /* RLCA RRCA RLA RRA: the CB rotate of A, Z cleared */
    movs    r0, #0x18
    ands    r0, r7
    ldrb    r1, [r4, #R_A]
    bl      gbl_cbop
    strb    r0, [r4, #R_A]
    movs    r0, #1
    strb    r0, [r4, #R_ZR]
    b       .Lnext

H_DI:
    movs    r0, #0
    strb    r0, [r4, #R_IME]
    b       .Lnext

H_CPL:
    ldrb    r0, [r4, #R_A]
    mvns    r0, r0
    strb    r0, [r4, #R_A]
    movs    r0, #1
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_HF]
    b       .Lnext

H_SCF:
    movs    r0, #1
    strb    r0, [r4, #R_CF]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_HF]
    b       .Lnext

H_CCF:
    ldrb    r0, [r4, #R_CF]
    movs    r1, #1
    eors    r0, r1
    strb    r0, [r4, #R_CF]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_HF]
    b       .Lnext

H_LDSPHL:
    ldrh    r0, [r4, #R_HL]
    strh    r0, [r4, #R_SP]
    b       .Lnext

    .align 2
.Lt:
    .hword (H_NOP - .Lt) / 2, (H_LDRRNN - .Lt) / 2, (H_ST_BC - .Lt) / 2, (H_INCRR - .Lt) / 2, (H_INC - .Lt) / 2, (H_DEC - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_ROTA - .Lt) / 2
    .hword (H_C - .Lt) / 2, (H_ADDHL - .Lt) / 2, (H_LD_BC - .Lt) / 2, (H_DECRR - .Lt) / 2, (H_INC - .Lt) / 2, (H_DEC - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_ROTA - .Lt) / 2
    .hword (H_C - .Lt) / 2, (H_LDRRNN - .Lt) / 2, (H_ST_DE - .Lt) / 2, (H_INCRR - .Lt) / 2, (H_INC - .Lt) / 2, (H_DEC - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_ROTA - .Lt) / 2
    .hword (H_JR - .Lt) / 2, (H_ADDHL - .Lt) / 2, (H_LD_DE - .Lt) / 2, (H_DECRR - .Lt) / 2, (H_INC - .Lt) / 2, (H_DEC - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_ROTA - .Lt) / 2
    .hword (H_JRCC - .Lt) / 2, (H_LDRRNN - .Lt) / 2, (H_LDI_ST - .Lt) / 2, (H_INCRR - .Lt) / 2, (H_INC - .Lt) / 2, (H_DEC - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_C - .Lt) / 2
    .hword (H_JRCC - .Lt) / 2, (H_ADDHL - .Lt) / 2, (H_LDI_LD - .Lt) / 2, (H_DECRR - .Lt) / 2, (H_INC - .Lt) / 2, (H_DEC - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_CPL - .Lt) / 2
    .hword (H_JRCC - .Lt) / 2, (H_LDRRNN - .Lt) / 2, (H_LDD_ST - .Lt) / 2, (H_INCRR - .Lt) / 2, (H_INC - .Lt) / 2, (H_DEC - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_SCF - .Lt) / 2
    .hword (H_JRCC - .Lt) / 2, (H_ADDHL - .Lt) / 2, (H_LDD_LD - .Lt) / 2, (H_DECRR - .Lt) / 2, (H_INC - .Lt) / 2, (H_DEC - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_CCF - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_C - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2
    .hword (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2
    .hword (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2
    .hword (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2
    .hword (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2
    .hword (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2
    .hword (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2
    .hword (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2, (H_ALUR - .Lt) / 2
    .hword (H_RETCC - .Lt) / 2, (H_POP - .Lt) / 2, (H_JPCC - .Lt) / 2, (H_JP - .Lt) / 2, (H_CALLCC - .Lt) / 2, (H_PUSH - .Lt) / 2, (H_ALUN - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_RETCC - .Lt) / 2, (H_RET - .Lt) / 2, (H_JPCC - .Lt) / 2, (H_CB - .Lt) / 2, (H_CALLCC - .Lt) / 2, (H_CALL - .Lt) / 2, (H_ALUN - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_RETCC - .Lt) / 2, (H_POP - .Lt) / 2, (H_JPCC - .Lt) / 2, (H_C - .Lt) / 2, (H_CALLCC - .Lt) / 2, (H_PUSH - .Lt) / 2, (H_ALUN - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_RETCC - .Lt) / 2, (H_C - .Lt) / 2, (H_JPCC - .Lt) / 2, (H_C - .Lt) / 2, (H_CALLCC - .Lt) / 2, (H_C - .Lt) / 2, (H_ALUN - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_LDH_ST - .Lt) / 2, (H_POP - .Lt) / 2, (H_LDC_ST - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_PUSH - .Lt) / 2, (H_ALUN - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_C - .Lt) / 2, (H_JPHL - .Lt) / 2, (H_ST_NN - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_ALUN - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_LDH_LD - .Lt) / 2, (H_C - .Lt) / 2, (H_LDC_LD - .Lt) / 2, (H_DI - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_ALUN - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_C - .Lt) / 2, (H_LDSPHL - .Lt) / 2, (H_LD_NN - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_ALUN - .Lt) / 2, (H_RST - .Lt) / 2
.Lreg_offsets:
    .byte   1, 0, 3, 2, 5, 4, 0xFF, R_A   /* B C D E H L (HL) A */
    .size gbl_interp_block, . - gbl_interp_block

#endif
