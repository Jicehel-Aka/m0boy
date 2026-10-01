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
 * The table decodes what it can: each condition, ALU operation and
 * register / (HL) / immediate operand has its own handler, so handlers do
 * not test opcode fields at run time.  Operands are read by the handlers
 * that need them, through r6.
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

/* gbl_interp_ctx_t (gbl_internal.h): register offsets, then the innermost
 * gbl_exec's bound */
#define SP0 8
#define BOUNDED 10

/* r4 = gbr, r5 = instructions not yet added to gbl_interp_instrs,
 * r6 = address of the opcode (operands at r6 + 1, r6 + 2; a copy when the
 * fetch went through gbl_rd), r7 = opcode, r8 = gbl_interp_ctx (register offsets first),
 * r9 = gbl_cyc_main, r10 = gbl_rmap, r11 = dispatch table.  r0-r3 are
 * scratch and do not survive calls. */

/* r0 = gbr offset of the register in op bits \shift.. (B C D E H L - A) */
.macro REGOFS shift
.if \shift == 0
    movs    r0, #7
    ands    r0, r7
.else
    lsrs    r0, r7, #3
    movs    r1, #7
    ands    r0, r1
.endif
    mov     r1, r8
    ldrb    r0, [r1, r0]
.endm

/* same for op bits 3-5 of an opcode below 0x40 */
.macro REGOFS_LO
    lsrs    r0, r7, #3
    mov     r1, r8
    ldrb    r0, [r1, r0]
.endm

/* \reg = n16 (\tmp clobbered) */
.macro N16 reg, tmp
    ldrb    \reg, [r6, #1]
    ldrb    \tmp, [r6, #2]
    lsls    \tmp, \tmp, #8
    orrs    \reg, \tmp
.endm

/* gbr.pc += n (r0 = the new pc, not truncated) */
.macro PC_ADD n
    ldrh    r0, [r4, #R_PC]
    adds    r0, #\n
    strh    r0, [r4, #R_PC]
.endm

/* cycles += n */
.macro CYC_ADD n
    ldr     r0, [r4, #R_CYC]
    adds    r0, #\n
    str     r0, [r4, #R_CYC]
.endm

/* r0 = byte at 0xFF00 + r0, as gbl_rd: HRAM directly, IO through
 * gbl_io_rd, IE through gbl_rd */
.macro RD_FF
    cmp     r0, #0x80
    blo     81f
    cmp     r0, #0xFF
    beq     82f
    subs    r0, #0x80
    ldr     r1, =gbl_hram
    ldrb    r0, [r1, r0]
    b       83f
81:
    bl      gbl_io_rd
    b       83f
82:
    ldr     r0, =0xFFFF
    bl      gbl_rd
83:
.endm

/* byte at 0xFF00 + r0 = r1, as gbl_wr */
.macro WR_FF
    cmp     r0, #0x80
    blo     81f
    cmp     r0, #0xFF
    beq     82f
    subs    r0, #0x80
    ldr     r2, =gbl_hram
    strb    r1, [r2, r0]
    b       83f
81:
    bl      gbl_io_wr
    b       83f
82:
    ldr     r0, =0xFFFF
    bl      gbl_wr
83:
.endm


/* The loop top (.Ltop .. dispatch) inline at the end of the hottest
 * handlers: saves the branch back to .Ltop. */
.macro NEXT
    ldr     r1, [r4, #R_CYC]
    ldr     r0, [r4, #R_NEXT]
    subs    r0, r1, r0
    bpl     97f
    ldrh    r0, [r4, #R_PC]
    lsrs    r2, r0, #12
    lsls    r2, r2, #2
    mov     r3, r10
    ldr     r3, [r3, r2]
    cmp     r3, #0
    beq     98f
    lsls    r2, r0, #20
    asrs    r2, r2, #21
    adds    r2, #1
    beq     98f
    adds    r6, r3, r0
    ldrb    r7, [r6]
    adds    r0, #1
    strh    r0, [r4, #R_PC]
    mov     r2, r9
    ldrb    r2, [r2, r7]
    adds    r1, r1, r2
    str     r1, [r4, #R_CYC]
    adds    r5, #1
    lsls    r0, r7, #1
    mov     r1, r11
    ldrsh   r0, [r1, r0]
    lsls    r0, r0, #1
    adds    r0, r0, r1
    mov     pc, r0
97:
    b       .Levent
98:
    b       .Lslow_fetch
.endm

    .section .text.gbl_interp_block, "ax", %progbits
    .align 2
/* Handlers placed before the loop, so that every handler stays within
 * branch range of .Ltop (the hot ones follow the loop). */
    .type gbl_interp_handlers, %function
gbl_interp_handlers:
/* -- 16-bit -- */

H_LDRRNN:
    N16     r1, r2
    lsrs    r0, r7, #4
    lsls    r0, r0, #1
    strh    r1, [r4, r0]
    PC_ADD  2
    b       .Ltop

H_INCRR:
    lsrs    r0, r7, #4
    lsls    r0, r0, #1
    ldrh    r1, [r4, r0]
    adds    r1, #1
    strh    r1, [r4, r0]
    b       .Ltop

H_DECRR:
    lsrs    r0, r7, #4
    lsls    r0, r0, #1
    ldrh    r1, [r4, r0]
    subs    r1, #1
    strh    r1, [r4, r0]
    b       .Ltop

H_ADDHL:
    lsrs    r0, r7, #4
    lsls    r0, r0, #1
    ldrh    r0, [r4, r0]
    bl      gbl_addhl
    b       .Ltop

H_LDSPHL:
    ldrh    r0, [r4, #R_HL]
    strh    r0, [r4, #R_SP]
    b       .Ltop

H_PUSH:
    lsrs    r0, r7, #4
    movs    r1, #3
    ands    r0, r1
    lsls    r0, r0, #1
    ldrh    r0, [r4, r0]
    bl      gbl_push
    b       .Ltop

H_POP:
    bl      gbl_pop
    lsrs    r1, r7, #4
    movs    r2, #3
    ands    r1, r2
    lsls    r1, r1, #1
    strh    r0, [r4, r1]
    b       .Ltop

/* -- control transfers: a handler per condition -- */

H_JR:
    movs    r1, #1
    ldrsb   r1, [r6, r1]
    ldrh    r0, [r4, #R_PC]
    adds    r0, #1
    adds    r0, r0, r1
    strh    r0, [r4, #R_PC]
    b       .Ljump

H_JRZ:
    ldrb    r0, [r4, #R_ZR]
    cmp     r0, #0
    beq     .Ljr_take
    b       .Ljr_skip
H_JRNC:
    ldrb    r0, [r4, #R_CF]
    cmp     r0, #0
    beq     .Ljr_take
    b       .Ljr_skip
H_JRC:
    ldrb    r0, [r4, #R_CF]
    cmp     r0, #0
    bne     .Ljr_take
.Ljr_skip:
    PC_ADD  1
    NEXT
H_JRNZ:
    ldrb    r0, [r4, #R_ZR]
    cmp     r0, #0
    beq     .Ljr_skip
#if GBL_INTERP_DELAY_LOOP
    ldrb    r0, [r6, #1]           /* JR NZ,-3: the C shortcut of DEC A loops */
    cmp     r0, #0xFD
    bne     .Ljr_take
    b       H_C
#endif
.Ljr_take:
    movs    r1, #1
    ldrsb   r1, [r6, r1]
    ldrh    r0, [r4, #R_PC]
    adds    r0, #1
    adds    r0, r0, r1
    strh    r0, [r4, #R_PC]
    CYC_ADD 4
    b       .Ljump

H_JP:
    N16     r0, r1
    strh    r0, [r4, #R_PC]
    b       .Ljump

H_JPZ:
    ldrb    r0, [r4, #R_ZR]
    cmp     r0, #0
    beq     .Ljp_take
    b       .Ljp_skip
H_JPNC:
    ldrb    r0, [r4, #R_CF]
    cmp     r0, #0
    beq     .Ljp_take
    b       .Ljp_skip
H_JPC:
    ldrb    r0, [r4, #R_CF]
    cmp     r0, #0
    bne     .Ljp_take
    b       .Ljp_skip
H_JPNZ:
    ldrb    r0, [r4, #R_ZR]
    cmp     r0, #0
    bne     .Ljp_take
.Ljp_skip:
    PC_ADD  2
    b       .Ltop
.Ljp_take:
    N16     r0, r1
    strh    r0, [r4, #R_PC]
    CYC_ADD 4
    b       .Ljump

H_JPHL:
    ldrh    r0, [r4, #R_HL]
    strh    r0, [r4, #R_PC]
    b       .Ljump

H_CALL:
    N16     r7, r1
    PC_ADD  2
    uxth    r0, r0
    bl      gbl_push
    strh    r7, [r4, #R_PC]
    b       .Ljump

H_CALLZ:
    ldrb    r0, [r4, #R_ZR]
    cmp     r0, #0
    beq     .Lcall_take
    b       .Lcall_skip
H_CALLNC:
    ldrb    r0, [r4, #R_CF]
    cmp     r0, #0
    beq     .Lcall_take
    b       .Lcall_skip
H_CALLC:
    ldrb    r0, [r4, #R_CF]
    cmp     r0, #0
    bne     .Lcall_take
    b       .Lcall_skip
H_CALLNZ:
    ldrb    r0, [r4, #R_ZR]
    cmp     r0, #0
    bne     .Lcall_take
.Lcall_skip:
    PC_ADD  2
    b       .Ltop
.Lcall_take:
    N16     r7, r1
    PC_ADD  2
    uxth    r0, r0
    bl      gbl_push
    strh    r7, [r4, #R_PC]
    CYC_ADD 12
    b       .Ljump

H_RET:
    bl      gbl_pop
    strh    r0, [r4, #R_PC]
    b       .Ljump

H_RETZ:
    ldrb    r0, [r4, #R_ZR]
    cmp     r0, #0
    beq     .Lretcc_take
    b       .Ltop
H_RETNC:
    ldrb    r0, [r4, #R_CF]
    cmp     r0, #0
    beq     .Lretcc_take
    b       .Ltop
H_RETC:
    ldrb    r0, [r4, #R_CF]
    cmp     r0, #0
    bne     .Lretcc_take
    b       .Ltop
H_RETNZ:
    ldrb    r0, [r4, #R_ZR]
    cmp     r0, #0
    bne     .Lretcc_take
    b       .Ltop
.Lretcc_take:
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

/* -- CB prefix: BIT, RES, SET here, rotates/shifts/SWAP via gbl_cbop -- */

H_CB:
    ldrb    r7, [r6, #1]           /* CB opcode */
    PC_ADD  1
    movs    r0, #7
    ands    r0, r7
    cmp     r0, #6
    beq     .Lcb_hl
    ldr     r1, [r4, #R_CYC]       /* extra cycles: 4 */
    adds    r1, #4
    str     r1, [r4, #R_CYC]
    mov     r1, r8
    ldrb    r6, [r1, r0]           /* register offset */
    ldrb    r2, [r4, r6]
    lsrs    r1, r7, #6
    beq     5f
    lsrs    r3, r7, #3
    movs    r0, #7
    ands    r3, r0
    movs    r0, #1
    lsls    r0, r3                 /* bit mask */
    cmp     r1, #2
    blo     .Lcb_bit
    beq     3f
    orrs    r2, r0                 /* SET */
    strb    r2, [r4, r6]
    b       .Ltop
3:
    bics    r2, r0                 /* RES */
    strb    r2, [r4, r6]
    b       .Ltop
5:
    mov     r1, r2
    mov     r0, r7
    bl      gbl_cbop
    strb    r0, [r4, r6]
    b       .Ltop
.Lcb_bit:  /* r0 = mask, r2 = value: Z from the bit, N 0, H 1 */
    ands    r0, r2
    strb    r0, [r4, #R_ZR]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    movs    r0, #1
    strb    r0, [r4, #R_HF]
    NEXT
.Lcb_hl:  /* extra cycles: 12, BIT n,(HL) 8 */
    movs    r1, #12
    lsrs    r2, r7, #6
    cmp     r2, #1
    bne     1f
    movs    r1, #8
1:
    ldr     r2, [r4, #R_CYC]
    adds    r2, r2, r1
    str     r2, [r4, #R_CYC]
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    mov     r2, r0
    lsrs    r1, r7, #6
    beq     5f
    lsrs    r3, r7, #3
    movs    r0, #7
    ands    r3, r0
    movs    r0, #1
    lsls    r0, r3
    cmp     r1, #2
    blo     .Lcb_bit
    beq     3f
    orrs    r2, r0
    b       6f
3:
    bics    r2, r0
6:
    mov     r1, r2
    ldrh    r0, [r4, #R_HL]
    bl      gbl_wr
    b       .Ltop
5:
    mov     r1, r2
    mov     r0, r7
    bl      gbl_cbop
    mov     r1, r0
    ldrh    r0, [r4, #R_HL]
    bl      gbl_wr
    b       .Ltop

/* -- the rest -- */

H_ROTA:  /* RLCA RRCA RLA RRA: the CB rotate of A, Z cleared */
    movs    r0, #0x18
    ands    r0, r7
    ldrb    r1, [r4, #R_A]
    bl      gbl_cbop
    strb    r0, [r4, #R_A]
    movs    r0, #1
    strb    r0, [r4, #R_ZR]
    b       .Ltop

H_DI:
    movs    r0, #0
    strb    r0, [r4, #R_IME]
    b       .Ltop

H_CPL:
    ldrb    r0, [r4, #R_A]
    mvns    r0, r0
    strb    r0, [r4, #R_A]
    movs    r0, #1
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_HF]
    b       .Ltop

H_SCF:
    movs    r0, #1
    strb    r0, [r4, #R_CF]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_HF]
    b       .Ltop

H_CCF:
    ldrb    r0, [r4, #R_CF]
    movs    r1, #1
    eors    r0, r1
    strb    r0, [r4, #R_CF]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_HF]
    b       .Ltop

    .size gbl_interp_handlers, . - gbl_interp_handlers

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
    ldr     r0, .Lcyc
    mov     r9, r0
    ldr     r0, .Lrmap
    mov     r10, r0
    ldr     r0, .Ltable
    mov     r11, r0
    movs    r5, #0
.Ltop:
    ldr     r1, [r4, #R_CYC]
    ldr     r0, [r4, #R_NEXT]
    subs    r0, r1, r0
    bpl     .Levent
.Lfetch_cyc:  /* r1 = gbr.cyc */
    ldrh    r0, [r4, #R_PC]
    lsrs    r2, r0, #12
    lsls    r2, r2, #2
    mov     r3, r10
    ldr     r3, [r3, r2]
    cmp     r3, #0
    beq     .Lslow_fetch
    lsls    r2, r0, #20            /* opcode and operands in one page? */
    asrs    r2, r2, #21            /* -1 when pc & 0xFFE == 0xFFE */
    adds    r2, #1
    beq     .Lslow_fetch
    adds    r6, r3, r0
    ldrb    r7, [r6]
.Lfetched:  /* r0 = pc, r1 = gbr.cyc */
    adds    r0, #1
    strh    r0, [r4, #R_PC]
    mov     r2, r9
    ldrb    r2, [r2, r7]
    adds    r1, r1, r2
    str     r1, [r4, #R_CYC]
    adds    r5, #1
    lsls    r0, r7, #1
    mov     r1, r11
    ldrsh   r0, [r1, r0]           /* handlers precede the table */
    lsls    r0, r0, #1
    adds    r0, r0, r1
    mov     pc, r0

.Levent:
    ldr     r0, .Linstrs           /* the frontend reads it during gbl_sync */
    ldr     r1, [r0]
    adds    r1, r1, r5
    str     r1, [r0]
    movs    r5, #0
    bl      gbl_sync
    bl      gbl_irq_enter
    cmp     r0, #0
    bne     .Lret
.Lfetch:  /* also after EI: the next instruction runs before any interrupt */
    ldr     r1, [r4, #R_CYC]
    b       .Lfetch_cyc

.Lslow_fetch:  /* same reads as the C loop, in the same order */
    ldr     r6, .Lbuf
    mov     r7, r0
    bl      gbl_rd
    strb    r0, [r6]
    adds    r0, r7, #1
    uxth    r0, r0
    bl      gbl_rd
    strb    r0, [r6, #1]
    adds    r0, r7, #2
    uxth    r0, r0
    bl      gbl_rd
    strb    r0, [r6, #2]
    mov     r0, r7
    ldrb    r7, [r6]
    ldr     r1, [r4, #R_CYC]
    b       .Lfetched

.Ljump:  /* after a control transfer */
    /* Start a new block here when the dispatcher would call this function
     * again: gbl_exec's frame not left (its SP bound), and no compiled code
     * at the target.  Same block boundaries, without the return, the
     * dispatcher loop and the call. */
    mov     r3, r8
    ldrb    r0, [r3, #BOUNDED]
    cmp     r0, #0
    beq     1f
    ldrh    r0, [r3, #SP0]
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
.Lret:  /* gbl_interp_instrs += instructions since the last event */
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
.Lregofs:   .word gbl_interp_ctx
.Lrmap:     .word gbl_rmap
.Ltable:    .word .Lt
.Lcyc:      .word gbl_cyc_main
.Linstrs:   .word gbl_interp_instrs
.Lbuf:      .word gbl_interp_fetch_buf
.Linterp_only: .word gbl_interp_only

/* ---- handlers (offsets from the dispatch table at the end) ---- */

H_C:  /* the other opcodes: C, one instruction */
    N16     r1, r2
    mov     r0, r7
    bl      gbl_interp_op
    lsrs    r1, r0, #1             /* C = control transfer, r1 = EI */
    bcs     1f
    cmp     r1, #0
    bne     2f
H_NOP:
    b       .Ltop
1:
    b       .Ljump
2:
    b       .Lfetch

/* -- 8-bit loads -- */

H_LDRR:  /* LD r, r' */
    REGOFS  0
    ldrb    r2, [r4, r0]
    REGOFS  3
    strb    r2, [r4, r0]
    NEXT

H_LDR_HL:  /* LD r, (HL) */
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    mov     r2, r0
    REGOFS  3
    strb    r2, [r4, r0]
    NEXT

H_LDHL_R:  /* LD (HL), r */
    REGOFS  0
    ldrb    r1, [r4, r0]
    ldrh    r0, [r4, #R_HL]
    bl      gbl_wr
    b       .Ltop

H_LDRN:  /* LD r, n */
    ldrb    r2, [r6, #1]
    REGOFS_LO
    strb    r2, [r4, r0]
    PC_ADD  1
    NEXT

H_LDRN_HL:  /* LD (HL), n */
    ldrb    r1, [r6, #1]
    ldrh    r0, [r4, #R_HL]
    bl      gbl_wr
    PC_ADD  1
    b       .Ltop

H_ST_BC:
    ldrh    r0, [r4, #R_BC]
    b       1f
H_ST_DE:
    ldrh    r0, [r4, #R_DE]
1:
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    b       .Ltop

H_LD_BC:
    ldrh    r0, [r4, #R_BC]
    b       1f
H_LD_DE:
    ldrh    r0, [r4, #R_DE]
1:
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    b       .Ltop

H_LDI_ST:
    ldrh    r0, [r4, #R_HL]
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    ldrh    r0, [r4, #R_HL]
    adds    r0, #1
    strh    r0, [r4, #R_HL]
    b       .Ltop

H_LDD_ST:
    ldrh    r0, [r4, #R_HL]
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    ldrh    r0, [r4, #R_HL]
    subs    r0, #1
    strh    r0, [r4, #R_HL]
    b       .Ltop

H_LDI_LD:
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    ldrh    r0, [r4, #R_HL]
    adds    r0, #1
    strh    r0, [r4, #R_HL]
    NEXT

H_LDD_LD:
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    ldrh    r0, [r4, #R_HL]
    subs    r0, #1
    strh    r0, [r4, #R_HL]
    b       .Ltop

H_LDH_ST:  /* LDH (n), A */
    ldrb    r0, [r6, #1]
    ldrb    r1, [r4, #R_A]
    WR_FF
    PC_ADD  1
    b       .Ltop

H_LDH_LD:  /* LDH A, (n) */
    ldrb    r0, [r6, #1]
    RD_FF
    strb    r0, [r4, #R_A]
    PC_ADD  1
    NEXT

H_LDC_ST:  /* LD (C), A */
    ldrb    r0, [r4, #R_BC]
    ldrb    r1, [r4, #R_A]
    WR_FF
    b       .Ltop

H_LDC_LD:  /* LD A, (C) */
    ldrb    r0, [r4, #R_BC]
    RD_FF
    strb    r0, [r4, #R_A]
    b       .Ltop
    .ltorg

H_ST_NN:
    N16     r0, r1
    ldrb    r1, [r4, #R_A]
    bl      gbl_wr
    PC_ADD  2
    b       .Ltop

H_LD_NN:
    N16     r0, r1
    bl      gbl_rd
    strb    r0, [r4, #R_A]
    PC_ADD  2
    b       .Ltop

/* -- ALU: an entry per operand kind, r2 = operand -- */

.macro ALU_SRC op
H_ALUR_\op:
    REGOFS  0
    ldrb    r2, [r4, r0]
    b       1f
H_ALUHL_\op:
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    mov     r2, r0
    b       1f
H_ALUN_\op:
    ldrb    r2, [r6, #1]
    PC_ADD  1
1:
.endm

/* Flags as gbl_add .. gbl_cp of gbl_core_asm.S (every input checked there
 * by BENCH_ALU_CHECK), inline. */
    ALU_SRC ADD
    ldrb    r1, [r4, #R_A]
    lsls    r3, r1, #28
    lsrs    r3, r3, #28
    adds    r1, r1, r2             /* a + v */
    lsls    r2, r2, #28
    lsrs    r2, r2, #28
    adds    r3, r3, r2
    lsrs    r3, r3, #4             /* half carry */
    strb    r3, [r4, #R_HF]
    lsrs    r3, r1, #8             /* carry */
    strb    r3, [r4, #R_CF]
    strb    r1, [r4, #R_A]
    strb    r1, [r4, #R_ZR]
    movs    r3, #0
    strb    r3, [r4, #R_NF]
    NEXT

    ALU_SRC ADC
    ldrb    r1, [r4, #R_A]
    ldrb    r0, [r4, #R_CF]
    lsls    r3, r1, #28
    lsrs    r3, r3, #28
    adds    r3, r3, r0
    adds    r1, r1, r0
    adds    r1, r1, r2             /* a + v + c */
    lsls    r2, r2, #28
    lsrs    r2, r2, #28
    adds    r3, r3, r2
    lsrs    r3, r3, #4
    strb    r3, [r4, #R_HF]
    lsrs    r3, r1, #8
    strb    r3, [r4, #R_CF]
    strb    r1, [r4, #R_A]
    strb    r1, [r4, #R_ZR]
    movs    r3, #0
    strb    r3, [r4, #R_NF]
    b       .Ltop

    ALU_SRC SUB
    ldrb    r1, [r4, #R_A]
    lsls    r3, r1, #28
    lsrs    r3, r3, #28
    subs    r1, r1, r2             /* a - v */
    lsls    r2, r2, #28
    lsrs    r2, r2, #28
    subs    r3, r3, r2
    lsrs    r3, r3, #31            /* half borrow */
    strb    r3, [r4, #R_HF]
    lsrs    r3, r1, #31            /* borrow */
    strb    r3, [r4, #R_CF]
    strb    r1, [r4, #R_A]
    strb    r1, [r4, #R_ZR]
    movs    r3, #1
    strb    r3, [r4, #R_NF]
    NEXT

    ALU_SRC SBC
    ldrb    r1, [r4, #R_A]
    ldrb    r0, [r4, #R_CF]
    lsls    r3, r1, #28
    lsrs    r3, r3, #28
    subs    r3, r3, r0
    subs    r1, r1, r0
    subs    r1, r1, r2             /* a - v - c */
    lsls    r2, r2, #28
    lsrs    r2, r2, #28
    subs    r3, r3, r2
    lsrs    r3, r3, #31
    strb    r3, [r4, #R_HF]
    lsrs    r3, r1, #31
    strb    r3, [r4, #R_CF]
    strb    r1, [r4, #R_A]
    strb    r1, [r4, #R_ZR]
    movs    r3, #1
    strb    r3, [r4, #R_NF]
    b       .Ltop

    ALU_SRC AND
    ldrb    r0, [r4, #R_A]
    ands    r0, r2
    strb    r0, [r4, #R_A]
    strb    r0, [r4, #R_ZR]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_CF]
    movs    r0, #1
    strb    r0, [r4, #R_HF]
    NEXT

    ALU_SRC XOR
    ldrb    r0, [r4, #R_A]
    eors    r0, r2
    b       .Llogic

    ALU_SRC OR
    ldrb    r0, [r4, #R_A]
    orrs    r0, r2
.Llogic:  /* XOR, OR */
    strb    r0, [r4, #R_A]
    strb    r0, [r4, #R_ZR]
    movs    r0, #0
    strb    r0, [r4, #R_NF]
    strb    r0, [r4, #R_HF]
    strb    r0, [r4, #R_CF]
    NEXT

    ALU_SRC CP
    ldrb    r1, [r4, #R_A]
    subs    r3, r1, r2
    strb    r3, [r4, #R_ZR]
    sbcs    r3, r3                 /* -1 on borrow */
    negs    r3, r3
    strb    r3, [r4, #R_CF]
    lsls    r1, r1, #28
    lsls    r2, r2, #28
    subs    r1, r1, r2             /* low nibbles */
    sbcs    r3, r3
    negs    r3, r3
    strb    r3, [r4, #R_HF]
    movs    r3, #1
    strb    r3, [r4, #R_NF]
    NEXT

/* -- INC / DEC -- */

H_INC_R:
    REGOFS_LO
    ldrb    r2, [r4, r0]
    lsls    r1, r2, #28
    lsrs    r1, r1, #28
    adds    r1, #1
    lsrs    r1, r1, #4             /* (v & 15) == 15 */
    strb    r1, [r4, #R_HF]
    movs    r1, #0
    strb    r1, [r4, #R_NF]
    adds    r2, #1
    strb    r2, [r4, r0]
    strb    r2, [r4, #R_ZR]
    NEXT

H_DEC_R:
    REGOFS_LO
    ldrb    r2, [r4, r0]
    lsls    r1, r2, #28
    lsrs    r1, r1, #28
    subs    r1, #1
    lsrs    r1, r1, #31            /* (v & 15) == 0 */
    strb    r1, [r4, #R_HF]
    movs    r1, #1
    strb    r1, [r4, #R_NF]
    subs    r2, #1
    strb    r2, [r4, r0]
    strb    r2, [r4, #R_ZR]
    NEXT

H_INC_HL:
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    lsls    r1, r0, #28
    lsrs    r1, r1, #28
    adds    r1, #1
    lsrs    r1, r1, #4
    strb    r1, [r4, #R_HF]
    movs    r1, #0
    strb    r1, [r4, #R_NF]
    adds    r1, r0, #1
    b       1f

H_DEC_HL:
    ldrh    r0, [r4, #R_HL]
    bl      gbl_rd
    lsls    r1, r0, #28
    lsrs    r1, r1, #28
    subs    r1, #1
    lsrs    r1, r1, #31
    strb    r1, [r4, #R_HF]
    movs    r1, #1
    strb    r1, [r4, #R_NF]
    subs    r1, r0, #1
1:
    uxtb    r1, r1
    strb    r1, [r4, #R_ZR]
    ldrh    r0, [r4, #R_HL]
    bl      gbl_wr
    b       .Ltop

    .align 2
.Lt:
    .hword (H_NOP - .Lt) / 2, (H_LDRRNN - .Lt) / 2, (H_ST_BC - .Lt) / 2, (H_INCRR - .Lt) / 2, (H_INC_R - .Lt) / 2, (H_DEC_R - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_ROTA - .Lt) / 2
    .hword (H_C - .Lt) / 2, (H_ADDHL - .Lt) / 2, (H_LD_BC - .Lt) / 2, (H_DECRR - .Lt) / 2, (H_INC_R - .Lt) / 2, (H_DEC_R - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_ROTA - .Lt) / 2
    .hword (H_C - .Lt) / 2, (H_LDRRNN - .Lt) / 2, (H_ST_DE - .Lt) / 2, (H_INCRR - .Lt) / 2, (H_INC_R - .Lt) / 2, (H_DEC_R - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_ROTA - .Lt) / 2
    .hword (H_JR - .Lt) / 2, (H_ADDHL - .Lt) / 2, (H_LD_DE - .Lt) / 2, (H_DECRR - .Lt) / 2, (H_INC_R - .Lt) / 2, (H_DEC_R - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_ROTA - .Lt) / 2
    .hword (H_JRNZ - .Lt) / 2, (H_LDRRNN - .Lt) / 2, (H_LDI_ST - .Lt) / 2, (H_INCRR - .Lt) / 2, (H_INC_R - .Lt) / 2, (H_DEC_R - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_C - .Lt) / 2
    .hword (H_JRZ - .Lt) / 2, (H_ADDHL - .Lt) / 2, (H_LDI_LD - .Lt) / 2, (H_DECRR - .Lt) / 2, (H_INC_R - .Lt) / 2, (H_DEC_R - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_CPL - .Lt) / 2
    .hword (H_JRNC - .Lt) / 2, (H_LDRRNN - .Lt) / 2, (H_LDD_ST - .Lt) / 2, (H_INCRR - .Lt) / 2, (H_INC_HL - .Lt) / 2, (H_DEC_HL - .Lt) / 2, (H_LDRN_HL - .Lt) / 2, (H_SCF - .Lt) / 2
    .hword (H_JRC - .Lt) / 2, (H_ADDHL - .Lt) / 2, (H_LDD_LD - .Lt) / 2, (H_DECRR - .Lt) / 2, (H_INC_R - .Lt) / 2, (H_DEC_R - .Lt) / 2, (H_LDRN - .Lt) / 2, (H_CCF - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDR_HL - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDR_HL - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDR_HL - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDR_HL - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDR_HL - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDR_HL - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_LDHL_R - .Lt) / 2, (H_LDHL_R - .Lt) / 2, (H_LDHL_R - .Lt) / 2, (H_LDHL_R - .Lt) / 2, (H_LDHL_R - .Lt) / 2, (H_LDHL_R - .Lt) / 2, (H_C - .Lt) / 2, (H_LDHL_R - .Lt) / 2
    .hword (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDRR - .Lt) / 2, (H_LDR_HL - .Lt) / 2, (H_LDRR - .Lt) / 2
    .hword (H_ALUR_ADD - .Lt) / 2, (H_ALUR_ADD - .Lt) / 2, (H_ALUR_ADD - .Lt) / 2, (H_ALUR_ADD - .Lt) / 2, (H_ALUR_ADD - .Lt) / 2, (H_ALUR_ADD - .Lt) / 2, (H_ALUHL_ADD - .Lt) / 2, (H_ALUR_ADD - .Lt) / 2
    .hword (H_ALUR_ADC - .Lt) / 2, (H_ALUR_ADC - .Lt) / 2, (H_ALUR_ADC - .Lt) / 2, (H_ALUR_ADC - .Lt) / 2, (H_ALUR_ADC - .Lt) / 2, (H_ALUR_ADC - .Lt) / 2, (H_ALUHL_ADC - .Lt) / 2, (H_ALUR_ADC - .Lt) / 2
    .hword (H_ALUR_SUB - .Lt) / 2, (H_ALUR_SUB - .Lt) / 2, (H_ALUR_SUB - .Lt) / 2, (H_ALUR_SUB - .Lt) / 2, (H_ALUR_SUB - .Lt) / 2, (H_ALUR_SUB - .Lt) / 2, (H_ALUHL_SUB - .Lt) / 2, (H_ALUR_SUB - .Lt) / 2
    .hword (H_ALUR_SBC - .Lt) / 2, (H_ALUR_SBC - .Lt) / 2, (H_ALUR_SBC - .Lt) / 2, (H_ALUR_SBC - .Lt) / 2, (H_ALUR_SBC - .Lt) / 2, (H_ALUR_SBC - .Lt) / 2, (H_ALUHL_SBC - .Lt) / 2, (H_ALUR_SBC - .Lt) / 2
    .hword (H_ALUR_AND - .Lt) / 2, (H_ALUR_AND - .Lt) / 2, (H_ALUR_AND - .Lt) / 2, (H_ALUR_AND - .Lt) / 2, (H_ALUR_AND - .Lt) / 2, (H_ALUR_AND - .Lt) / 2, (H_ALUHL_AND - .Lt) / 2, (H_ALUR_AND - .Lt) / 2
    .hword (H_ALUR_XOR - .Lt) / 2, (H_ALUR_XOR - .Lt) / 2, (H_ALUR_XOR - .Lt) / 2, (H_ALUR_XOR - .Lt) / 2, (H_ALUR_XOR - .Lt) / 2, (H_ALUR_XOR - .Lt) / 2, (H_ALUHL_XOR - .Lt) / 2, (H_ALUR_XOR - .Lt) / 2
    .hword (H_ALUR_OR - .Lt) / 2, (H_ALUR_OR - .Lt) / 2, (H_ALUR_OR - .Lt) / 2, (H_ALUR_OR - .Lt) / 2, (H_ALUR_OR - .Lt) / 2, (H_ALUR_OR - .Lt) / 2, (H_ALUHL_OR - .Lt) / 2, (H_ALUR_OR - .Lt) / 2
    .hword (H_ALUR_CP - .Lt) / 2, (H_ALUR_CP - .Lt) / 2, (H_ALUR_CP - .Lt) / 2, (H_ALUR_CP - .Lt) / 2, (H_ALUR_CP - .Lt) / 2, (H_ALUR_CP - .Lt) / 2, (H_ALUHL_CP - .Lt) / 2, (H_ALUR_CP - .Lt) / 2
    .hword (H_RETNZ - .Lt) / 2, (H_POP - .Lt) / 2, (H_JPNZ - .Lt) / 2, (H_JP - .Lt) / 2, (H_CALLNZ - .Lt) / 2, (H_PUSH - .Lt) / 2, (H_ALUN_ADD - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_RETZ - .Lt) / 2, (H_RET - .Lt) / 2, (H_JPZ - .Lt) / 2, (H_CB - .Lt) / 2, (H_CALLZ - .Lt) / 2, (H_CALL - .Lt) / 2, (H_ALUN_ADC - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_RETNC - .Lt) / 2, (H_POP - .Lt) / 2, (H_JPNC - .Lt) / 2, (H_C - .Lt) / 2, (H_CALLNC - .Lt) / 2, (H_PUSH - .Lt) / 2, (H_ALUN_SUB - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_RETC - .Lt) / 2, (H_C - .Lt) / 2, (H_JPC - .Lt) / 2, (H_C - .Lt) / 2, (H_CALLC - .Lt) / 2, (H_C - .Lt) / 2, (H_ALUN_SBC - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_LDH_ST - .Lt) / 2, (H_POP - .Lt) / 2, (H_LDC_ST - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_PUSH - .Lt) / 2, (H_ALUN_AND - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_C - .Lt) / 2, (H_JPHL - .Lt) / 2, (H_ST_NN - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_ALUN_XOR - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_LDH_LD - .Lt) / 2, (H_C - .Lt) / 2, (H_LDC_LD - .Lt) / 2, (H_DI - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_ALUN_OR - .Lt) / 2, (H_RST - .Lt) / 2
    .hword (H_C - .Lt) / 2, (H_LDSPHL - .Lt) / 2, (H_LD_NN - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_C - .Lt) / 2, (H_ALUN_CP - .Lt) / 2, (H_RST - .Lt) / 2
    .size gbl_interp_block, . - gbl_interp_block

    .section .bss.gbl_interp_fetch_buf, "aw", %nobits
    .align 2
gbl_interp_fetch_buf:  /* opcode and operands read through gbl_rd */
    .space 4

#endif
