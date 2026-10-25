/* via_tick and via_write in Thumb-1 (Cortex-M0+).  via_tick: via.c's
 * advance() and settle() — the timers, the shift register's bits, the
 * beam's straight moves, the analog signature after an event (derive)
 * and the stroke it ends (via_flush, in C).  via_write: the ports, T1's
 * high byte and the shift register (nineteen writes in twenty), via_sync
 * included; the other registers go to via.c's via_write_c.  The VIA's
 * fields are reached from one register.
 * via.c holds the C version (the reference, and every other target's);
 * its _Static_asserts pin the offsets below.  Checked as the project's
 * other asm paths: the same strokes (VX_DRAW_HASH) and the same screen as
 * the C build in gm0, game frame for game frame. */
#include "vx_via_asm.h"
#if VX_VIA_ASM

	.syntax unified
	.cpu cortex-m0plus
	.thumb

/* VIA fields, from &v->via (vectrex.h) */
#define ORB 1
#define ACR 4
#define PCR 5
#define IFR 6
#define SR 8
#define T1 10
#define T1_LATCH 12
#define T2 14
#define T1_ARMED 17
#define T2_ARMED 18
#define XSH 19
#define YSH 20
#define RSH 21
#define INTENSITY 22
#define SR_BITS 23
#define SR_WAIT 24
#define CB2 25
#define RAMP 26
#define BEAM_X 28
#define BEAM_Y 32
#define PEN_CYCLES 44
#define PEN_SIG 48
#define TIME 80
#define CALM 88
#define VEL_X 92
#define VEL_Y 96
#define SIG 100
#define ORA 0
#define PEN_X 36
#define PEN_Y 40
#define AY_REG 54
#define AY 55
#define AY_EPOCH 72
#define ACCESSES 84
#define VIA_IN_VECTREX 36
/* M6809, at the start of Vectrex */
#define CPU_CYCLES 16
#define CPU_START 20
#define CPU_PC 12
#define IER 7
#define JOY_X 1753
#define JOY_Y 1754
#define ATTENTION 1761

/* the beam's straight move for \n cycles while the ramp runs, the
 * stroke's length, T1's count (r0-r2 clobbered) */
.macro MOVE n
	ldrb	r0, [r4, #RAMP]
	cmp	r0, #0
	beq	1f
	ldrb	r1, [r4, #RSH]
	ldrb	r0, [r4, #XSH]
	subs	r0, r0, r1
	muls	r0, \n
	lsls	r0, r0, #2		@ VX_SLEW
	ldr	r2, [r4, #BEAM_X]
	adds	r2, r2, r0
	str	r2, [r4, #BEAM_X]
	ldrb	r0, [r4, #YSH]
	subs	r0, r0, r1
	muls	r0, \n
	lsls	r0, r0, #2
	ldr	r2, [r4, #BEAM_Y]
	subs	r2, r2, r0		@ positive Y is up
	str	r2, [r4, #BEAM_Y]
1:
	ldr	r0, [r4, #PEN_CYCLES]
	adds	r0, r0, \n
	str	r0, [r4, #PEN_CYCLES]
	ldrh	r0, [r4, #T1]
	subs	r0, r0, \n
	strh	r0, [r4, #T1]
.endm

	.text
	.align	1

/* derive(p = r4): the signature analog() computes, and the ramp bit
 * (r0-r3 clobbered) */
	.thumb_func
	.type	vx_derive_asm, %function
vx_derive_asm:
	ldrb	r0, [r4, #PCR]
	lsrs	r1, r0, #1
	movs	r2, #7
	ands	r1, r2
	movs	r3, #0
	cmp	r1, #6
	bne	.Ld_beam
	movs	r3, #2			@ zeroing: 0x200, neither lit nor ramping
	lsls	r3, r3, #8
	movs	r2, #0
	b	.Ld_store
.Ld_beam:
	ldrb	r1, [r4, #ACR]
	/* lit: ACR bit 4 ? CB2 : PCR's CB2 control held high (7) */
	lsls	r2, r1, #27
	bpl	.Ld_pcr
	ldrb	r2, [r4, #CB2]
	b	.Ld_on
.Ld_pcr:
	lsrs	r0, r0, #5
	movs	r2, #0
	cmp	r0, #7
	bne	.Ld_on
	movs	r2, #1
.Ld_on:
	cmp	r2, #0
	beq	.Ld_ramp
	ldrb	r2, [r4, #INTENSITY]
	cmp	r2, #0
	beq	.Ld_ramp
	lsls	r2, r2, #1		@ on | intensity << 1
	adds	r2, r2, #1
	orrs	r3, r2
.Ld_ramp:
	/* ramping: ACR bit 7 ? T1 running : ORB bit 7 low */
	lsls	r2, r1, #24
	bpl	.Ld_orb
	ldrb	r2, [r4, #T1_ARMED]
	b	.Ld_r
.Ld_orb:
	ldrb	r2, [r4, #ORB]
	lsrs	r2, r2, #7
	movs	r0, #1
	eors	r2, r0
.Ld_r:
	cmp	r2, #0
	beq	.Ld_store
	movs	r0, #1			@ 0x100, and the velocity's two terms
	lsls	r0, r0, #8
	orrs	r3, r0
	ldrb	r1, [r4, #RSH]
	ldrb	r0, [r4, #XSH]
	subs	r0, r0, r1
	lsls	r0, r0, #23		@ & 0x1ff, << 10
	lsrs	r0, r0, #13
	orrs	r3, r0
	ldrb	r0, [r4, #YSH]
	subs	r0, r0, r1
	lsls	r0, r0, #23		@ & 0x1ff, << 19
	lsrs	r0, r0, #4
	orrs	r3, r0
.Ld_store:
	str	r3, [r4, #SIG]
	strb	r2, [r4, #RAMP]
	bx	lr
	.size	vx_derive_asm, .-vx_derive_asm

/* via_sync(v = r6, v->cpu.start), p = r4 (r0-r3 clobbered): within the
 * quiet time a straight move, else via_tick */
	.thumb_func
	.type	vx_sync_asm, %function
vx_sync_asm:
	ldr	r0, [r6, #CPU_START]
	ldr	r1, [r4, #TIME]
	subs	r1, r0, r1
	ble	.Ls_done
	ldr	r2, [r4, #CALM]
	cmp	r1, r2
	bcs	.Ls_tick
	subs	r2, r2, r1
	str	r2, [r4, #CALM]
	str	r0, [r4, #TIME]
	ldrh	r0, [r4, #T1]
	subs	r0, r0, r1
	strh	r0, [r4, #T1]
	ldrh	r0, [r4, #T2]
	subs	r0, r0, r1
	strh	r0, [r4, #T2]
	ldr	r0, [r4, #PEN_CYCLES]
	adds	r0, r0, r1
	str	r0, [r4, #PEN_CYCLES]
	ldr	r0, [r4, #VEL_X]
	muls	r0, r1
	ldr	r2, [r4, #BEAM_X]
	adds	r2, r2, r0
	str	r2, [r4, #BEAM_X]
	ldr	r0, [r4, #VEL_Y]
	muls	r0, r1
	ldr	r2, [r4, #BEAM_Y]
	adds	r2, r2, r0
	str	r2, [r4, #BEAM_Y]
.Ls_done:
	bx	lr
.Ls_tick:
	movs	r0, r6			@ r1 = n
	ldr	r2, =via_tick
	bx	r2			@ returns to the caller
	.size	vx_sync_asm, .-vx_sync_asm

/* settle_velocity(p = r4) (r0-r1 clobbered) */
	.thumb_func
	.type	vx_velocity_asm, %function
vx_velocity_asm:
	ldrb	r0, [r4, #RAMP]
	cmp	r0, #0
	beq	.Lv_still
	ldrb	r1, [r4, #RSH]
	ldrb	r0, [r4, #XSH]
	subs	r0, r0, r1
	lsls	r0, r0, #2
	str	r0, [r4, #VEL_X]
	ldrb	r0, [r4, #YSH]
	subs	r0, r1, r0
	lsls	r0, r0, #2
	str	r0, [r4, #VEL_Y]
	bx	lr
.Lv_still:
	str	r0, [r4, #VEL_X]
	str	r0, [r4, #VEL_Y]
	bx	lr
	.size	vx_velocity_asm, .-vx_velocity_asm

/* settle_calm(p = r4): the quiet time, T1's and T2's time-outs and the
 * shift register's next bit (r0-r2 clobbered) */
	.thumb_func
	.type	vx_calm_asm, %function
vx_calm_asm:
	movs	r0, #0
	mvns	r0, r0
	ldrb	r1, [r4, #T1_ARMED]
	cmp	r1, #0
	beq	.Lcalm_t2
	ldrh	r0, [r4, #T1]
	adds	r0, r0, #1
.Lcalm_t2:
	ldrb	r1, [r4, #T2_ARMED]
	cmp	r1, #0
	beq	.Lcalm_sr
	ldrh	r1, [r4, #T2]
	adds	r1, r1, #1
	cmp	r1, r0
	bcs	.Lcalm_sr
	movs	r0, r1
.Lcalm_sr:
	ldrb	r1, [r4, #ACR]
	movs	r2, #0x1c
	ands	r1, r2
	cmp	r1, #0x18
	bne	.Lcalm_set
	ldrb	r1, [r4, #SR_BITS]
	cmp	r1, #8
	bcs	.Lcalm_set
	ldrb	r1, [r4, #SR_WAIT]
	cmp	r1, r0
	bcs	.Lcalm_set
	movs	r0, r1
.Lcalm_set:
	str	r0, [r4, #CALM]
	bx	lr
	.size	vx_calm_asm, .-vx_calm_asm

/* void via_tick(Vectrex *v, unsigned n)
 * r4 = &v->via, r5 = cycles left, r6 = v, r7 = the step; r3 within a
 * step: bit 0 T1 times out, bit 1 the shift register runs, bits 8-11 the
 * shift register's event (its bit count) */
	.global	via_tick
	.thumb_func
	.type	via_tick, %function
via_tick:
	push	{r4, r5, r6, r7, lr}
	movs	r6, r0
	movs	r4, r0
	adds	r4, r4, #VIA_IN_VECTREX
	movs	r5, r1
	ldr	r0, [r4, #TIME]
	adds	r0, r0, r5
	str	r0, [r4, #TIME]
	/* T2 only raises its flag */
	ldrb	r0, [r4, #T2_ARMED]
	ldrh	r1, [r4, #T2]
	cmp	r0, #0
	beq	.Lt2
	cmp	r5, r1
	bls	.Lt2
	ldrb	r0, [r4, #IFR]
	movs	r2, #0x20
	orrs	r0, r2
	strb	r0, [r4, #IFR]
	movs	r0, #0
	strb	r0, [r4, #T2_ARMED]
.Lt2:
	subs	r1, r1, r5
	strh	r1, [r4, #T2]
	/* no T1 time-out and no shifting within n: one straight move */
	ldrb	r0, [r4, #T1_ARMED]
	cmp	r0, #0
	beq	.Lno_t1
	ldrh	r0, [r4, #T1]
	cmp	r0, r5
	bcc	.Lloop
.Lno_t1:
	ldrb	r0, [r4, #ACR]
	movs	r1, #0x1c
	ands	r0, r1
	cmp	r0, #0x18
	bne	.Lstraight
	ldrb	r0, [r4, #SR_BITS]
	cmp	r0, #8
	bcc	.Lloop
.Lstraight:
	MOVE	r5
	b	.Lsettle

.Lloop:
	cmp	r5, #0
	bne	.Lstep
	b	.Lsettle
.Lstep:
	movs	r7, r5
	movs	r3, #0
	ldrb	r0, [r4, #T1_ARMED]
	cmp	r0, #0
	beq	.Lsr
	ldrh	r0, [r4, #T1]
	cmp	r0, r7
	bcs	.Lsr
	adds	r0, r0, #1		@ up to T1's time-out
	movs	r7, r0
	movs	r3, #1
.Lsr:
	ldrb	r0, [r4, #ACR]
	movs	r1, #0x1c
	ands	r0, r1
	cmp	r0, #0x18
	bne	.Lmove
	ldrb	r1, [r4, #SR_BITS]
	cmp	r1, #8
	bcs	.Lmove
	adds	r3, r3, #2
	movs	r0, #8
	subs	r1, r0, r1		@ the bits left
	/* 1 + the bits from bit 7 down equal to CB2, at most those left */
	ldrb	r0, [r4, #CB2]
	rsbs	r0, r0, #0
	ldrb	r2, [r4, #SR]
	eors	r0, r2
	uxtb	r0, r0
	ldr	r2, =vx_sr_lead
	ldrb	r0, [r2, r0]
	adds	r0, r0, #1
	cmp	r0, r1
	bls	.Le
	movs	r0, r1
.Le:
	subs	r1, r0, #1		@ its time: sr_wait + 2 (e - 1)
	lsls	r1, r1, #1
	ldrb	r2, [r4, #SR_WAIT]
	adds	r1, r1, r2
	cmp	r1, r7
	bhi	.Lmove
	beq	.Lsr_step
	movs	r2, #1			@ before T1's: T1 runs on
	bics	r3, r2
.Lsr_step:
	movs	r7, r1
	lsls	r0, r0, #8
	orrs	r3, r0
.Lmove:
	MOVE	r7
	subs	r5, r5, r7
	movs	r0, #2
	tst	r3, r0
	beq	.Lt1_event
	/* the shift register's bits within the step: all of them up to the
	 * event, or those before the step's end */
	lsrs	r0, r3, #8
	bne	.Ldone
	ldrb	r1, [r4, #SR_WAIT]
	cmp	r7, r1
	bcc	.Lsr_wait		@ none yet: r0 = 0
	subs	r0, r7, r1
	lsrs	r0, r0, #1
	adds	r0, r0, #1
.Ldone:
	mov	ip, r3
	ldrb	r1, [r4, #SR]
	movs	r2, #8
	subs	r2, r2, r0
	movs	r3, r1
	lsrs	r3, r2			@ the bits shifted out, CB2 the last
	lsls	r1, r0
	orrs	r1, r3
	strb	r1, [r4, #SR]		@ rotated
	movs	r2, #1
	ands	r3, r2
	strb	r3, [r4, #CB2]
	ldrb	r1, [r4, #SR_BITS]
	adds	r1, r1, r0
	strb	r1, [r4, #SR_BITS]
	mov	r3, ip
	cmp	r1, #8
	bne	.Lsr_wait
	ldrb	r1, [r4, #IFR]
	movs	r2, #4
	orrs	r1, r2
	strb	r1, [r4, #IFR]
.Lsr_wait:
	lsrs	r1, r3, #8
	beq	.Lsr_wait_left
	movs	r1, #2
	b	.Lsr_wait_set
.Lsr_wait_left:
	ldrb	r1, [r4, #SR_WAIT]	@ sr_wait + 2 done - step
	lsls	r2, r0, #1
	adds	r1, r1, r2
	subs	r1, r1, r7
.Lsr_wait_set:
	strb	r1, [r4, #SR_WAIT]
.Lt1_event:
	movs	r0, #1
	tst	r3, r0
	beq	.Ltouch
	ldrb	r0, [r4, #IFR]
	movs	r1, #0x40
	orrs	r0, r1
	strb	r0, [r4, #IFR]
	ldrb	r0, [r4, #ACR]
	tst	r0, r1
	beq	.Lt1_once
	ldrh	r0, [r4, #T1_LATCH]	@ free running: reloaded
	strh	r0, [r4, #T1]
	b	.Ltouch
.Lt1_once:
	movs	r0, #0
	strb	r0, [r4, #T1_ARMED]
.Ltouch:
	/* an event: the stroke ends where the analog state changed */
	movs	r0, #2
	bics	r3, r0
	beq	.Lnext			@ (.Lloop is out of a conditional branch's reach)
	bl	vx_derive_asm
	ldr	r0, [r4, #SIG]
	ldr	r1, [r4, #PEN_SIG]
	cmp	r0, r1
	beq	.Lnext
	movs	r0, r6
	bl	via_flush
.Lnext:
	b	.Lloop

.Lsettle:
	bl	vx_velocity_asm
	bl	vx_calm_asm
	pop	{r4, r5, r6, r7, pc}
	.ltorg
	.size	via_tick, .-via_tick

/* void via_write(Vectrex *v, unsigned reg, uint8_t data)
 * r4 = &v->via, r5 = reg, r6 = v, r7 = data (then: redo the stroke's
 * state), r3 = shape: what analog() reads may have changed */
	.global	via_write
	.thumb_func
	.type	via_write, %function
via_write:
	cmp	r1, #2
	bcc	.Lw_fast		@ ORB, ORA
	cmp	r1, #5
	beq	.Lw_fast
	cmp	r1, #10
	beq	.Lw_fast
	cmp	r1, #15
	beq	.Lw_fast
	ldr	r3, =via_write_c
	bx	r3
.Lw_fast:
	push	{r4, r5, r6, r7, lr}
	movs	r6, r0
	movs	r4, r0
	adds	r4, r4, #VIA_IN_VECTREX
	movs	r5, r1
	movs	r7, r2
	bl	vx_sync_asm
	ldr	r0, [r4, #ACCESSES]
	adds	r0, r0, #1
	str	r0, [r4, #ACCESSES]
	movs	r3, #0
	cmp	r5, #10
	bne	.Lw_not_sr
	b	.Lw_sr			@ out of a conditional branch's reach
.Lw_not_sr:
	cmp	r5, #5
	beq	.Lw_t1h
	cmp	r5, #0
	beq	.Lw_orb
	/* ORA: the DAC; it moves the beam while the ramp runs */
	strb	r7, [r4, #ORA]
	movs	r0, #0x80
	eors	r0, r7
	ldrb	r1, [r4, #XSH]
	strb	r0, [r4, #XSH]
	ldrb	r2, [r4, #RAMP]
	cmp	r2, #0
	beq	.Lw_sample
	cmp	r0, r1
	beq	.Lw_sample
	movs	r3, #1
	b	.Lw_sample
.Lw_orb:
	/* ORB: bit 7 is the ramp when T1 does not drive it */
	ldrb	r1, [r4, #ORB]
	strb	r7, [r4, #ORB]
	eors	r1, r7
	lsls	r1, r1, #24
	bpl	.Lw_sample
	ldrb	r0, [r4, #ACR]
	lsls	r0, r0, #24
	bmi	.Lw_sample
	movs	r3, #1
.Lw_sample:
	/* the multiplexer's sample-and-hold, when enabled (ORB bit 0 low) */
	ldrb	r0, [r4, #ORB]
	lsrs	r1, r0, #1
	bcs	.Lw_sound
	movs	r0, #3
	ands	r0, r1
	ldrb	r1, [r4, #XSH]
	cmp	r0, #1
	beq	.Lw_rsh
	bhi	.Lw_z
	ldrb	r2, [r4, #YSH]
	strb	r1, [r4, #YSH]
	b	.Lw_held
.Lw_rsh:
	ldrb	r2, [r4, #RSH]
	strb	r1, [r4, #RSH]
	b	.Lw_held
.Lw_z:
	cmp	r0, #3
	beq	.Lw_sound
	subs	r1, #0x80		@ brightness: xsh - 0x80, at least 0
	bgt	.Lw_bright
	movs	r1, #0
.Lw_bright:
	ldrb	r2, [r4, #INTENSITY]
	strb	r1, [r4, #INTENSITY]
.Lw_held:
	cmp	r1, r2
	beq	.Lw_sound
	movs	r3, #1
.Lw_sound:
	/* the PSG's bus: BDIR/BC1 on ORB bits 4 and 3 */
	ldrb	r0, [r4, #ORB]
	movs	r1, #0x18
	ands	r0, r1
	cmp	r0, #0x18
	bne	.Lw_psg_write
	ldrb	r1, [r4, #ORA]		@ latch the register number
	movs	r2, #15
	ands	r1, r2
	movs	r2, #AY_REG		@ past ldrb/strb's immediate offsets
	strb	r1, [r4, r2]
	b	.Lw_shape
.Lw_psg_write:
	cmp	r0, #0x10
	bne	.Lw_shape
	movs	r2, #AY_REG
	ldrb	r1, [r4, r2]
	cmp	r1, #14
	beq	.Lw_shape
	adds	r2, r4, r1
	adds	r2, #AY
	ldrb	r0, [r4, #ORA]
	strb	r0, [r2]
	cmp	r1, #13
	bne	.Lw_shape
	ldr	r0, [r4, #AY_EPOCH]
	adds	r0, r0, #1
	str	r0, [r4, #AY_EPOCH]
.Lw_shape:
	/* redo the stroke's state when an input moved, or when the stroke
	 * lacks the current signature (after the reset) */
	movs	r7, #0
	cmp	r3, #0
	bne	.Lw_redo
	ldr	r0, [r4, #SIG]
	ldr	r1, [r4, #PEN_SIG]
	cmp	r0, r1
	beq	.Lw_zero
.Lw_redo:
	movs	r7, #1
	bl	vx_derive_asm
	ldr	r0, [r4, #SIG]
	ldr	r1, [r4, #PEN_SIG]
	cmp	r0, r1
	beq	.Lw_zero
	movs	r0, r6
	bl	via_flush
.Lw_zero:
	ldr	r0, [r4, #SIG]
	lsrs	r0, r0, #10		@ bit 9: zeroing, the beam at the centre
	bcc	.Lw_velocity
	movs	r0, #1
	lsls	r0, r0, #16		@ VX_CENTER
	str	r0, [r4, #BEAM_X]
	str	r0, [r4, #BEAM_Y]
	str	r0, [r4, #PEN_X]
	str	r0, [r4, #PEN_Y]
.Lw_velocity:
	cmp	r7, #0
	beq	.Lw_calm
	bl	vx_velocity_asm
.Lw_calm:
	cmp	r5, #5
	bne	.Lw_done
	bl	vx_calm_asm
.Lw_done:
	pop	{r4, r5, r6, r7, pc}
.Lw_t1h:
	/* T1's high byte: the latch, the count, T1 runs (the ramp with it
	 * under ACR bit 7), its flag cleared */
	ldrh	r0, [r4, #T1_LATCH]
	uxtb	r0, r0
	lsls	r1, r7, #8
	orrs	r0, r1
	strh	r0, [r4, #T1_LATCH]
	strh	r0, [r4, #T1]
	ldrb	r1, [r4, #T1_ARMED]
	cmp	r1, #0
	bne	.Lw_armed
	ldrb	r2, [r4, #ACR]
	lsls	r2, r2, #24
	bpl	.Lw_armed
	movs	r3, #1
.Lw_armed:
	movs	r1, #1
	strb	r1, [r4, #T1_ARMED]
	ldrb	r0, [r4, #IFR]
	movs	r1, #0x40
	bics	r0, r1
	strb	r0, [r4, #IFR]
	b	.Lw_shape
.Lw_sr:
	/* the shift register: eight bits from the next cycles */
	strb	r7, [r4, #SR]
	movs	r0, #0
	strb	r0, [r4, #SR_BITS]
	movs	r0, #2
	strb	r0, [r4, #SR_WAIT]
	ldrb	r0, [r4, #IFR]
	movs	r1, #4
	bics	r0, r1
	strb	r0, [r4, #IFR]
	bl	vx_calm_asm
	pop	{r4, r5, r6, r7, pc}
	.ltorg
	.size	via_write, .-via_write

/* uint8_t via_read(Vectrex *v, unsigned reg): ORB (the comparator) and
 * IFR (the spin detector first) here, the rest in via.c's via_read_c */
	.global	via_read
	.thumb_func
	.type	via_read, %function
via_read:
	cmp	r1, #0
	beq	.Lr_fast
	cmp	r1, #13
	beq	.Lr_fast
	ldr	r3, =via_read_c
	bx	r3
.Lr_fast:
	push	{r4, r5, r6, r7, lr}
	movs	r6, r0
	movs	r4, r0
	adds	r4, r4, #VIA_IN_VECTREX
	movs	r5, r1
	bl	vx_sync_asm
	ldr	r0, [r4, #ACCESSES]
	adds	r0, r0, #1
	str	r0, [r4, #ACCESSES]
	cmp	r5, #13
	beq	.Lr_ifr
	/* the comparator: the pot the mux selects against the DAC */
	ldrb	r0, [r4, #ORB]
	movs	r1, #6
	ldr	r2, =JOY_X
	tst	r0, r1
	beq	.Lr_pot
	adds	r2, r2, #1		@ JOY_Y
.Lr_pot:
	ldrb	r2, [r6, r2]
	movs	r1, #0x9f
	ands	r1, r0
	movs	r3, #0x40
	orrs	r1, r3
	ldrb	r3, [r4, #XSH]
	cmp	r2, r3
	bls	.Lr_pb7
	movs	r3, #0x20
	orrs	r1, r3
.Lr_pb7:
	/* PB7: T1's output under ACR bit 7 */
	ldrb	r3, [r4, #ACR]
	lsls	r3, r3, #24
	bpl	.Lr_ret
	movs	r3, #0x7f
	ands	r1, r3
	ldrb	r3, [r4, #T1_ARMED]
	cmp	r3, #0
	bne	.Lr_ret
	movs	r3, #0x80
	orrs	r1, r3
.Lr_ret:
	movs	r0, r1
	pop	{r4, r5, r6, r7, pc}
.Lr_ifr:
#ifndef VX_NO_SPIN
	movs	r0, r6
	bl	vx_skip_spin
#endif
	ldrb	r1, [r4, #IFR]
	ldrb	r2, [r4, #IER]
	ands	r2, r1
	movs	r3, #0x7f
	tst	r2, r3
	beq	.Lr_ret
	movs	r3, #0x80
	orrs	r1, r3
	b	.Lr_ret
	.ltorg
	.size	via_read, .-via_read

/* the recompiled code's slow paths (vx_gen.h, m6809_run.c's C otherwise):
 * the clock as the interpreter shows it, then the VIA directly */
	.global	vxg_read
	.thumb_func
	.type	vxg_read, %function
vxg_read:				@ (v, addr, start, info)
	str	r2, [r0, #CPU_START]
	mov	ip, r1
	lsls	r1, r3, #24		@ the cycles counted
	lsrs	r1, r1, #24
	adds	r2, r2, r1
	str	r2, [r0, #CPU_CYCLES]
	lsrs	r3, r3, #8		@ pc, when the read may be IFR's
	beq	.Lgr_pc
	strh	r3, [r0, #CPU_PC]
.Lgr_pc:
	mov	r1, ip
	lsrs	r2, r1, #11
	cmp	r2, #0x1a		@ D000-D7FF
	bne	.Lgr_bus
	movs	r2, #15
	ands	r1, r2
	b	via_read
.Lgr_bus:
	ldr	r3, =vx_read
	bx	r3
	.size	vxg_read, .-vxg_read

	.global	vxg_write
	.thumb_func
	.type	vxg_write, %function
vxg_write:				@ (v, addr | data << 16, start, counted)
	str	r2, [r0, #CPU_START]
	adds	r2, r2, r3
	str	r2, [r0, #CPU_CYCLES]
	push	{r4, lr}
	movs	r4, r0
	lsrs	r2, r1, #16
	uxth	r1, r1
	lsrs	r3, r1, #11
	cmp	r3, #0x1a
	bne	.Lgw_bus
	movs	r3, #15
	ands	r1, r3
	bl	via_write
	b	.Lgw_attention
.Lgw_bus:
	bl	vx_write
.Lgw_attention:
	ldr	r1, =ATTENTION
	ldrb	r0, [r4, r1]
	pop	{r4, pc}
	.ltorg
	.size	vxg_write, .-vxg_write

#endif
