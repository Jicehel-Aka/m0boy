/* 6522 VIA and the analog side it drives: the DAC, the sample-and-holds
 * behind the multiplexer, the X/Y integrators (the beam), ZERO, BLANK and
 * the PSG's bus.
 *
 * Time advances by whole instructions: between two writes the analog state
 * only changes when T1 times out (PB7, the ramp) or the shift register
 * moves a bit (CB2, the blanking), so via_tick() cuts the interval there.
 * The beam moves in straight lines in between; each straight stroke is one
 * vx_draw() call, made when the state shaping it changes (touch, via_flush):
 * a write that leaves the velocity, the blanking and the brightness alone
 * does not cut the stroke. */
#include "vectrex.h"
#include "vx_via_asm.h"
#include <stddef.h>

/* integrator speed: 1/256 pixel of the 512 grid per DAC step and cycle;
 * positive Y is up, the grid's rows go down */
#define VX_SLEW 4
/* the integrators saturate: the beam stays within a few screens */
#define VX_LIMIT (VX_GRID * 256 * 4)

static inline int zeroing(const VIA *p) { return ((p->pcr >> 1) & 7) == 6; }
static inline int lit(const VIA *p) {
  int on = (p->acr & 0x10) ? p->cb2 : ((p->pcr >> 5) & 7) == 7;
  return on && p->intensity && !zeroing(p);
}
/* the ramp is on while PB7 is low: T1's output, or ORB's bit 7 */
static inline int ramping(const VIA *p) {
  return (p->acr & 0x80) ? p->t1_armed : !(p->orb & 0x80);
}
/* what shapes the stroke: lit or not (and how bright), the beam held at
 * the centre, or its velocity while the ramp runs */
static inline uint32_t analog(const VIA *p) {
  const uint32_t on = (uint32_t)lit(p), zero = (uint32_t)zeroing(p);
  const uint32_t ramp = ramping(p) && !zero;
  uint32_t sig = on | (on ? (uint32_t)p->intensity << 1 : 0) | ramp << 8 | zero << 9;
  if (ramp)
    sig |= (uint32_t)(((int)p->xsh - (int)p->rsh) & 0x1ff) << 10 |
           (uint32_t)(((int)p->ysh - (int)p->rsh) & 0x1ff) << 19;
  return sig;
}
/* after a change of what analog() reads (ACR, PCR, ORB's bit 7, T1's
 * output, CB2, the sample-and-holds, the DAC): the cached signature */
static inline void derive(VIA *p) {
  p->sig = analog(p);
  p->ramp = (uint8_t)((p->sig >> 8) & 1u);
}
void via_derive(Vectrex *v) { derive(&v->via); }
/* after any change: the beam's velocity and the quiet time ahead (the
 * shift register's next bit, T1's and T2's time-outs) for via_sync */
static inline __attribute__((always_inline)) void settle_velocity(VIA *p) {
  const int ramp = p->ramp;
  p->vel_x = ramp ? ((int)p->xsh - (int)p->rsh) * VX_SLEW : 0;
  p->vel_y = ramp ? -((int)p->ysh - (int)p->rsh) * VX_SLEW : 0;
}
static inline __attribute__((always_inline)) void settle_calm(VIA *p) {
  uint32_t calm = 0xffffffffu;
  if (p->t1_armed)
    calm = p->t1 + 1u;
  if (p->t2_armed && p->t2 + 1u < calm)
    calm = p->t2 + 1u;
  if ((p->acr & 0x1c) == 0x18 && p->sr_bits < 8 && p->sr_wait < calm)
    calm = p->sr_wait;
  p->calm = calm;
}
static inline int32_t clamp(int32_t x) {
  return x < -VX_LIMIT ? -VX_LIMIT : x > VX_LIMIT ? VX_LIMIT : x;
}

/* end the current stroke here and start the next one */
void via_flush(Vectrex *v) {
  VIA *p = &v->via;
  if (p->pen_lit && p->pen_cycles) {
    vx_draw(v, p->pen_x, p->pen_y, p->beam_x, p->beam_y, p->pen_z,
            p->pen_cycles);
    p->segments++;
  }
  p->beam_x = clamp(p->beam_x);
  p->beam_y = clamp(p->beam_y);
  p->pen_x = p->beam_x;
  p->pen_y = p->beam_y;
  p->pen_cycles = 0;
  p->pen_lit = (uint8_t)(p->sig & 1u);
  p->pen_z = p->intensity;
  p->pen_sig = p->sig;
}
/* a new stroke only where the analog state changed */
static inline __attribute__((always_inline)) void touch(Vectrex *v) {
  derive(&v->via);
  if (v->via.sig != v->via.pen_sig)
    via_flush(v);
}

/* the multiplexer's sample-and-holds; nonzero when one of them changed */
static inline __attribute__((always_inline)) uint32_t sample(Vectrex *v) {
  VIA *p = &v->via;
  uint32_t old;
  if (p->orb & 1)
    return 0;
  switch ((p->orb >> 1) & 3) {
  case 0:
    old = p->ysh;
    p->ysh = p->xsh;
    return old ^ p->ysh;
  case 1:
    old = p->rsh;
    p->rsh = p->xsh;
    return old ^ p->rsh;
  case 2:
    old = p->intensity;
    p->intensity = p->xsh > 0x80 ? p->xsh - 0x80 : 0;
    return old ^ p->intensity;
  default:
    return 0;
  }
}
static inline __attribute__((always_inline)) void sound(VIA *p) {
  if ((p->orb & 0x18) == 0x18)
    p->ay_reg = p->ora & 15;
  if ((p->orb & 0x18) == 0x10 && p->ay_reg != 14) {
    p->ay[p->ay_reg] = p->ora;
    if (p->ay_reg == 13)
      p->ay_epoch++;
  }
}
/* an event only moves what analog() reads through CB2 (the blanking, when
 * ACR bit 4 takes it) or T1's output (the ramp, one-shot under ACR bit 7):
 * an event outside those cases leaves the signature alone */
static inline int event_touches_sig(const VIA *p, int t1_event, int cb2_moved) {
  if (t1_event && (p->acr & 0x80) && !(p->acr & 0x40))
    return 1;
  return (p->acr & 0x10) && cb2_moved;
}
/* cycles until the next IFR flag can rise (T1, T2, the shift register) */
static uint32_t next_event(const VIA *p) {
  uint32_t n = 0xffffffffu;
  if (p->t1_armed && p->t1 + 1u < n)
    n = p->t1 + 1u;
  if (p->t2_armed && p->t2 + 1u < n)
    n = p->t2 + 1u;
  if ((p->acr & 0x1c) == 0x18 && p->sr_bits < 8) {
    uint32_t s = p->sr_wait + 2u * (7u - p->sr_bits);
    if (s < n)
      n = s;
  }
  return n;
}
/* An IFR read at the same place as the previous one, the same value read,
 * the registers the same, no bus write and no other VIA access in between:
 * a polling loop without side effects.  Every iteration before the next flag would read
 * the same value, so they are skipped whole: the cycles and the VIA (the
 * beam included) advance as if they had run.  The register snapshot moves
 * only once the pc, the value and the bus witnesses have matched once (bit
 * 7 of spin_ifr, spare: the register's flags stop at bit 6): a scattered
 * IFR read costs four stores instead of thirteen, and a real loop skips
 * from its third read — the same cycles to the flag, the skip being a
 * whole count of the loop's period. */
void vx_skip_spin(Vectrex *v);
#define skip_spin vx_skip_spin /* via_asm.S calls it */
uint32_t vx_spin_dbg[6];
void vx_skip_spin(Vectrex *v) {
  VIA *p = &v->via;
  M6809 *c = &v->cpu;
  const uint8_t ifr = p->ifr;
  vx_spin_dbg[4]++;
  if (p->spin_pc != c->pc || (uint8_t)(p->spin_ifr & 0x7f) != ifr ||
      p->spin_accesses + 1 != p->accesses || p->spin_writes != c->writes) {
    p->spin_pc = c->pc;
    p->spin_ifr = ifr; /* bit 7 clear: not armed */
    p->spin_accesses = p->accesses;
    p->spin_writes = c->writes;
    return;
  }
  if (!(p->spin_ifr & 0x80)) {
    p->spin_ifr = (uint8_t)(ifr | 0x80);
    p->spin_accesses = p->accesses;
    p->spin_writes = c->writes;
    goto snapshot;
  }
  if (p->spin_a == c->a && p->spin_b == c->b && p->spin_x == c->x &&
      p->spin_y == c->y && p->spin_u == c->u && p->spin_s == c->s &&
      p->spin_dp == c->dp && p->spin_cc == c->cc &&
      !((p->ier & 0x7f) && !(c->cc & 0x10))) {
    const uint32_t period = c->cycles - p->spin_cycles;
    uint32_t room = next_event(p);
    /* not past the frame: the input and the sound change there */
    const uint32_t frame = v->frame_end + VX_FRAME_CYCLES - c->cycles;
    if ((int32_t)frame <= 0)
      room = 0;
    else if (frame < room)
      room = frame;
    if (period && period < 256 && room > period) {
      const uint32_t skip = (room - 1) / period * period;
      c->cycles += skip;
      c->start += skip;
      via_tick(v, skip);
      p->skipped += skip;
    }
  }
snapshot:
  p->spin_cycles = c->cycles;
  p->spin_a = c->a;
  p->spin_b = c->b;
  p->spin_x = c->x;
  p->spin_y = c->y;
  p->spin_u = c->u;
  p->spin_s = c->s;
  p->spin_dp = c->dp;
  p->spin_cc = c->cc;
}
#if VX_VIA_ASM
/* via_asm.S's via_read takes ORB and IFR, and hands the rest here */
uint8_t via_read_c(Vectrex *v, unsigned reg);
uint8_t via_read_c(Vectrex *v, unsigned reg) {
#else
uint8_t via_read(Vectrex *v, unsigned reg) {
#endif
  VIA *p = &v->via;
  uint8_t data = 0xff;
  via_sync(v, v->cpu.start);
  p->accesses++;
#ifndef VX_NO_SPIN
  if (reg == 13)
    skip_spin(v);
#endif
  switch (reg) {
  case 0: {
    /* the comparator: the pot selected by the mux against the DAC */
    uint8_t pot = ((p->orb >> 1) & 3) == 0 ? v->joy_x : v->joy_y;
    data = (p->orb & 0x9f) | 0x40 | (pot > p->xsh ? 0x20 : 0);
    if (p->acr & 0x80)
      data = (data & 0x7f) | (p->t1_armed ? 0 : 0x80);
    break;
  }
  case 1:
  case 15:
    data = (p->orb & 0x18) == 0x08 ? p->ay[p->ay_reg] : p->ora;
    break;
  case 2:
    data = p->ddrb;
    break;
  case 3:
    data = p->ddra;
    break;
  case 4:
    data = (uint8_t)p->t1;
    p->ifr &= ~0x40;
    break;
  case 5:
    data = p->t1 >> 8;
    break;
  case 6:
    data = (uint8_t)p->t1_latch;
    break;
  case 7:
    data = p->t1_latch >> 8;
    break;
  case 8:
    data = (uint8_t)p->t2;
    p->ifr &= ~0x20;
    break;
  case 9:
    data = p->t2 >> 8;
    break;
  case 10:
    data = p->sr;
    p->ifr &= ~0x04;
    break;
  case 11:
    data = p->acr;
    break;
  case 12:
    data = p->pcr;
    break;
  case 13:
    data = p->ifr | ((p->ifr & p->ier & 0x7f) ? 0x80 : 0);
    break;
  case 14:
    data = p->ier | 0x80;
    break;
  }
  return data;
}
#if VX_VIA_ASM
/* via_asm.S's via_write takes the ports, T1's high byte and the shift
 * register, and hands the rest here */
void via_write_c(Vectrex *v, unsigned reg, uint8_t data);
void via_write_c(Vectrex *v, unsigned reg, uint8_t data) {
#else
void via_write(Vectrex *v, unsigned reg, uint8_t data) {
#endif
  VIA *p = &v->via;
  /* nonzero when what analog() reads may have changed: ORB's bit 7 under
   * the ramp's control, the DAC while the ramp runs, a sample-and-hold,
   * T1 started under the ramp's control, ACR, PCR */
  uint32_t shape = 0;
  via_sync(v, v->cpu.start);
  p->accesses++;
  switch (reg) {
  case 0:
    shape = ((p->orb ^ data) & 0x80u) && !(p->acr & 0x80u);
    p->orb = data;
    shape |= sample(v);
    sound(p);
    break;
  case 1:
  case 15:
    p->ora = data;
    shape = p->ramp && p->xsh != (data ^ 0x80);
    p->xsh = data ^ 0x80;
    shape |= sample(v);
    sound(p);
    break;
  case 2:
    p->ddrb = data;
    break;
  case 3:
    p->ddra = data;
    break;
  case 4:
  case 6:
    p->t1_latch = (p->t1_latch & 0xff00) | data;
    break;
  case 5:
    p->t1_latch = (uint16_t)((p->t1_latch & 255) | (data << 8));
    p->t1 = p->t1_latch;
    shape = (p->acr & 0x80) && !p->t1_armed;
    p->t1_armed = 1;
    p->ifr &= ~0x40;
    break;
  case 7:
    p->t1_latch = (uint16_t)((p->t1_latch & 255) | (data << 8));
    p->ifr &= ~0x40;
    break;
  case 8:
    p->t2_latch = data;
    break;
  case 9:
    p->t2 = (uint16_t)(p->t2_latch | (data << 8));
    p->t2_armed = 1;
    p->ifr &= ~0x20;
    break;
  case 10:
    p->sr = data;
    p->sr_bits = 0;
    p->sr_wait = 2;
    p->ifr &= ~0x04;
    break;
  case 11:
    p->acr = data;
    shape = 1;
    break;
  case 12:
    p->pcr = data;
    shape = 1;
    break;
  case 13:
    p->ifr &= ~data;
    break;
  case 14:
    if (data & 0x80)
      p->ier |= data & 0x7f;
    else
      p->ier &= ~data;
    break;
  }
  /* the ports, T1's high byte (the ramp under T1), ACR and PCR shape the
   * beam; the latches, T2, SR, DDRs, IFR and IER do not.  Nothing to
   * redo when none of analog()'s inputs moved and the stroke already has
   * the current signature (it may not, right after the reset). */
  if ((1u << reg) & 0x9823u) {
    const uint32_t redo = shape || p->sig != p->pen_sig;
    if (redo)
      touch(v);
    if (p->sig & 0x200u) /* zeroing */
      p->beam_x = p->beam_y = p->pen_x = p->pen_y = VX_CENTER;
    /* the velocity: the ports, T1's high byte (the ramp under T1), ACR,
     * PCR */
    if (redo)
      settle_velocity(p);
  }
  /* the quiet time: T1's and T2's high bytes, SR, ACR */
  if ((1u << reg) & 0x0e20u)
    settle_calm(p);
  if (reg == 14)
    v->attention = (uint8_t)((p->ier & 0x7f) | v->cpu.waiting | v->fast_boot);
}
/* a byte's leading zero bits (ARMv6-M has no CLZ; via_asm.S reads it) */
extern const uint8_t vx_sr_lead[256];
const uint8_t vx_sr_lead[256] = {
#define L4(n) n, n, n, n
#define L8(n) L4(n), L4(n)
#define L16(n) L8(n), L8(n)
#define L32(n) L16(n), L16(n)
#define L64(n) L32(n), L32(n)
#define L128(n) L64(n), L64(n)
    8, 7, 6, 6, L4(5), L8(4), L16(3), L32(2), L64(1), L128(0)
#undef L4
#undef L8
#undef L16
#undef L32
#undef L64
#undef L128
};
#if VX_VIA_ASM
/* via_asm.S's offsets */
_Static_assert(offsetof(Vectrex, via) == 36 && offsetof(M6809, start) == 20, "VIA_IN_VECTREX, CPU_START");
_Static_assert(offsetof(VIA, ora) == 0 && offsetof(VIA, pen_x) == 36 && offsetof(VIA, pen_y) == 40 &&
                   offsetof(VIA, ay_reg) == 54 && offsetof(VIA, ay) == 55 && offsetof(VIA, ay_epoch) == 72 &&
                   offsetof(VIA, accesses) == 84 && VX_CENTER == 1 << 16,
               "via_asm.S, via_write's fields");
_Static_assert(offsetof(M6809, cycles) == 16 && offsetof(M6809, pc) == 12 && offsetof(VIA, ier) == 7 &&
                   offsetof(Vectrex, joy_x) == 1753 && offsetof(Vectrex, joy_y) == 1754 &&
                   offsetof(Vectrex, attention) == 1761,
               "via_asm.S, via_read's and vxg's fields");
_Static_assert(offsetof(VIA, orb) == 1 && offsetof(VIA, acr) == 4 && offsetof(VIA, pcr) == 5 &&
                   offsetof(VIA, ifr) == 6 && offsetof(VIA, sr) == 8 && offsetof(VIA, t1) == 10 &&
                   offsetof(VIA, t1_latch) == 12 && offsetof(VIA, t2) == 14 &&
                   offsetof(VIA, t1_armed) == 17 && offsetof(VIA, t2_armed) == 18 &&
                   offsetof(VIA, xsh) == 19 && offsetof(VIA, ysh) == 20 && offsetof(VIA, rsh) == 21 &&
                   offsetof(VIA, intensity) == 22 && offsetof(VIA, sr_bits) == 23 &&
                   offsetof(VIA, sr_wait) == 24 && offsetof(VIA, cb2) == 25 && offsetof(VIA, ramp) == 26,
               "via_asm.S byte fields");
_Static_assert(offsetof(VIA, beam_x) == 28 && offsetof(VIA, beam_y) == 32 &&
                   offsetof(VIA, pen_cycles) == 44 && offsetof(VIA, pen_sig) == 48 &&
                   offsetof(VIA, time) == 80 && offsetof(VIA, calm) == 88 && offsetof(VIA, vel_x) == 92 &&
                   offsetof(VIA, vel_y) == 96 && offsetof(VIA, sig) == 100,
               "via_asm.S word fields");
#else
static inline __attribute__((always_inline)) void advance(Vectrex *v, unsigned n);
void via_tick(Vectrex *v, unsigned n) {
  /* the velocity follows the ramp: nothing else in a tick moves it */
  const int ramp = v->via.ramp;
  advance(v, n);
  if (v->via.ramp != ramp)
    settle_velocity(&v->via);
  settle_calm(&v->via);
}
static inline __attribute__((always_inline)) void advance(Vectrex *v, unsigned n) {
  VIA *p = &v->via;
  p->time += n;
  /* T2 only raises its flag: the CPU sees it between instructions */
  if (p->t2_armed && n > p->t2) {
    p->ifr |= 0x20;
    p->t2_armed = 0;
  }
  p->t2 = (uint16_t)(p->t2 - n);
  /* no T1 time-out and no shifting within n: one straight move */
  if (!(p->t1_armed && p->t1 < n) && !((p->acr & 0x1c) == 0x18 && p->sr_bits < 8)) {
    if (p->ramp) {
      p->beam_x += (int32_t)n * ((int)p->xsh - (int)p->rsh) * VX_SLEW;
      p->beam_y -= (int32_t)n * ((int)p->ysh - (int)p->rsh) * VX_SLEW;
    }
    p->pen_cycles += n;
    p->t1 = (uint16_t)(p->t1 - n);
    return;
  }
  while (n) {
    unsigned step = n;
    int t1_event = 0;
    if (p->t1_armed && p->t1 < step) {
      step = p->t1 + 1u;
      t1_event = 1;
    }
    /* the shift register: a bit every 2 cycles, the first at sr_wait.
     * Only a change of CB2 (the blanking) or the last bit (the flag) is an
     * event: the bits equal to CB2 before it are applied in one go. */
    const int shifting = (p->acr & 0x1c) == 0x18 && p->sr_bits < 8;
    unsigned sr_event = 0, sr_at = 0, clamped = 0;
    if (shifting) {
      const unsigned left = 8u - p->sr_bits;
      /* 1 + the bits from bit 7 down equal to CB2, at most `left` */
      unsigned e = 1u + vx_sr_lead[p->sr ^ (p->cb2 ? 0xffu : 0u)];
      if (e > left) {
        e = left;
        clamped = 1; /* every bit left equals CB2: the blanking holds */
      }
      sr_at = p->sr_wait + 2u * (e - 1u);
      if (sr_at <= step) {
        if (sr_at < step)
          t1_event = 0;
        step = sr_at;
        sr_event = e;
      }
    }
    if (p->ramp) {
      p->beam_x += (int32_t)step * ((int)p->xsh - (int)p->rsh) * VX_SLEW;
      p->beam_y -= (int32_t)step * ((int)p->ysh - (int)p->rsh) * VX_SLEW;
    }
    p->pen_cycles += step;
    p->t1 = (uint16_t)(p->t1 - step);
    n -= step;
    if (shifting) {
      /* the shifts within the step: all of them up to the event, or those
       * before the step's end (CB2 unchanged by construction) */
      const unsigned done = sr_event ? sr_event
                            : step >= p->sr_wait ? 1u + (step - p->sr_wait) / 2u
                                                 : 0u;
      if (done) {
        p->cb2 = (p->sr >> (8u - done)) & 1u;
        p->sr = (uint8_t)(p->sr << done | p->sr >> (8u - done));
        p->sr_bits = (uint8_t)(p->sr_bits + done);
        if (p->sr_bits == 8)
          p->ifr |= 0x04;
      }
      p->sr_wait = sr_event ? 2u : (uint8_t)(p->sr_wait + 2u * done - step);
    }
    if (t1_event) {
      p->ifr |= 0x40;
      if (p->acr & 0x40)
        p->t1 = p->t1_latch;
      else
        p->t1_armed = 0;
    }
    if (t1_event || sr_event) {
      /* a stroke ends only where the analog state changed; an event that
       * cannot move it (a solid shift pattern, T1 free-running or not
       * driving the ramp) skips the derive and the compare whole */
      if (event_touches_sig(p, t1_event, (int)(sr_event && !clamped)))
        touch(v);
      else
        vx_spin_dbg[5]++;
    }
  }
}
#endif
