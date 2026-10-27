/* The 6809 run loop: m6809_step's semantics and cycles (m6809.c, the
 * reference) for the console's Cortex-M0+.  The registers stay in the
 * machine (one base register reaches them all: Thumb-1 has eight low
 * registers, locals spill anyway); pc and the clock are locals.  The bus
 * goes through the 1 KiB page maps (vx_reset): plain memory inline, the VIA
 * and the rest through vx_read/vx_write, pc and the clock stored first
 * (the VIA reads the clock, the spin detector the registers) and the clock
 * reloaded after (a skipped spin moves it).
 *
 * The rare and the odd (the 0x10/0x11 pages, EXG/TFR, SYNC, CWAI, RTI,
 * SWI, interrupts, code outside the ROMs and the RAM, illegal opcodes) are
 * left to the reference, one instruction at a time.
 * tests/test_vectrex_core.c runs both cores in lockstep on random code. */
#include "vectrex.h"

#define C 1u
#define V 2u
#define Z 4u
#define N 8u
#define H 32u

#define SAVE()                                                                 \
  do {                                                                         \
    c->pc = (uint16_t)pc;                                                      \
    c->cycles = cycles;                                                        \
    c->start = start;                                                          \
  } while (0)
#define RELOAD()                                                               \
  do {                                                                         \
    cycles = c->cycles;                                                        \
    start = c->start;                                                          \
  } while (0)

#define RD(addr_)                                                              \
  ({                                                                           \
    const uint32_t a_ = (addr_);                                               \
    const uint8_t *const m_ = rmap[a_ >> 10];                                  \
    uint32_t r_;                                                               \
    if (m_)                                                                    \
      r_ = m_[a_ & 1023u];                                                     \
    else {                                                                     \
      SAVE();                                                                  \
      r_ = vx_read(v, (uint16_t)a_);                                           \
      RELOAD();                                                                \
    }                                                                          \
    r_;                                                                        \
  })
#define WR(addr_, val_)                                                        \
  do {                                                                         \
    const uint32_t a_ = (addr_), d_ = (val_);                                  \
    uint8_t *const m_ = wmap[a_ >> 10];                                        \
    c->writes++;                                                               \
    if (m_)                                                                    \
      m_[a_ & 1023u] = (uint8_t)d_;                                            \
    else {                                                                     \
      SAVE();                                                                  \
      vx_write(v, (uint16_t)a_, (uint8_t)d_);                                  \
      RELOAD();                                                                \
    }                                                                          \
  } while (0)
#define RD16(addr_)                                                            \
  ({                                                                           \
    const uint32_t w_ = (addr_);                                               \
    const uint32_t h_ = RD(w_);                                                \
    (h_ << 8) | RD((w_ + 1u) & 0xffffu);                                       \
  })
#define WR16(addr_, val_)                                                      \
  do {                                                                         \
    const uint32_t w_ = (addr_), x_ = (val_);                                  \
    WR(w_, (x_ >> 8) & 0xffu);                                                 \
    WR((w_ + 1u) & 0xffffu, x_ & 0xffu);                                       \
  } while (0)
/* pc moves before the read, as m6809.c's rb(v, pc++) */
#define FETCH()                                                                \
  ({                                                                           \
    const uint32_t at_ = pc;                                                   \
    pc = (pc + 1u) & 0xffffu;                                                  \
    RD(at_);                                                                   \
  })
#define FETCH16()                                                              \
  ({                                                                           \
    const uint32_t g_ = FETCH();                                               \
    (g_ << 8) | FETCH();                                                       \
  })
#define PUSH8(field_, val_)                                                    \
  do {                                                                         \
    const uint32_t sp_ = (c->field_ - 1u) & 0xffffu;                           \
    c->field_ = (uint16_t)sp_;                                                 \
    WR(sp_, (val_) & 0xffu);                                                   \
  } while (0)
#define PUSH16(field_, val_)                                                   \
  do {                                                                         \
    const uint32_t p_ = (val_);                                                \
    PUSH8(field_, p_);                                                         \
    PUSH8(field_, p_ >> 8);                                                    \
  } while (0)
#define PULL8(field_)                                                          \
  ({                                                                           \
    const uint32_t sp_ = c->field_;                                            \
    c->field_ = (uint16_t)(sp_ + 1u);                                          \
    RD(sp_);                                                                   \
  })
#define PULL16(field_)                                                         \
  ({                                                                           \
    const uint32_t k_ = PULL8(field_);                                         \
    (k_ << 8) | PULL8(field_);                                                 \
  })

#define NZ8(r) (((r) & 0x80u ? N : 0) | ((r) ? 0 : Z))
#define NZ16(r) (((r) & 0x8000u ? N : 0) | ((r) ? 0 : Z))
#define SETNZ8(r) c->cc = (uint8_t)((c->cc & ~(N | Z | V)) | NZ8(r))
#define SETNZ16(r) c->cc = (uint8_t)((c->cc & ~(N | Z | V)) | NZ16(r))

static inline uint32_t add8(M6809 *c, uint32_t a, uint32_t b, uint32_t carry) {
  const uint32_t r = a + b + carry, z = r & 0xffu;
  c->cc = (uint8_t)((c->cc & ~(N | Z | V | C | H)) | NZ8(z) |
                    ((~(a ^ b) & (a ^ z) & 0x80u) ? V : 0) | (r >> 8) |
                    (((a & 15u) + (b & 15u) + carry) > 15u ? H : 0));
  return z;
}
static inline uint32_t sub8(M6809 *c, uint32_t a, uint32_t b, uint32_t carry) {
  const uint32_t r = a - b - carry, z = r & 0xffu;
  c->cc = (uint8_t)((c->cc & ~(N | Z | V | C)) | NZ8(z) |
                    (((a ^ b) & (a ^ z) & 0x80u) ? V : 0) | (r > 255u ? C : 0));
  return z;
}
static inline uint32_t sub16(M6809 *c, uint32_t a, uint32_t b) {
  const uint32_t r = a - b, z = r & 0xffffu;
  c->cc = (uint8_t)((c->cc & ~(N | Z | V | C)) | NZ16(z) |
                    (((a ^ b) & (a ^ z) & 0x8000u) ? V : 0) |
                    (r > 65535u ? C : 0));
  return z;
}
static inline uint32_t add16(M6809 *c, uint32_t a, uint32_t b) {
  const uint32_t r = a + b, z = r & 0xffffu;
  c->cc = (uint8_t)((c->cc & ~(N | Z | V | C)) | NZ16(z) |
                    ((~(a ^ b) & (a ^ z) & 0x8000u) ? V : 0) | (r >> 16));
  return z;
}
#define D() (((uint32_t)c->a << 8) | c->b)
#define SETD(val_)                                                             \
  do {                                                                         \
    const uint32_t e_ = (val_);                                                \
    c->a = (uint8_t)(e_ >> 8);                                                 \
    c->b = (uint8_t)e_;                                                        \
  } while (0)

/* NEG .. CLR (m6809.c: unary); 13 (TST) returns the operand */
static inline uint32_t unary(M6809 *c, uint32_t lo, uint32_t o) {
  uint32_t z = o, cc = c->cc;
  switch (lo) {
  case 0:
    return sub8(c, 0, o, 0);
  case 3:
    z = ~o & 0xffu;
    cc = (cc & ~(N | Z | V)) | NZ8(z) | C;
    break;
  case 4:
    z = o >> 1;
    cc = (cc & ~(N | Z | C)) | (z ? 0 : Z) | (o & 1u);
    break;
  case 6:
    z = (o >> 1) | ((cc & C) << 7);
    cc = (cc & ~(N | Z | C)) | NZ8(z) | (o & 1u);
    break;
  case 7:
    z = (o >> 1) | (o & 0x80u);
    cc = (cc & ~(N | Z | C)) | NZ8(z) | (o & 1u);
    break;
  case 8:
    z = (o << 1) & 0xffu;
    cc = (cc & ~(N | Z | V | C)) | NZ8(z) | (o >> 7) |
         (((o ^ z) & 0x80u) ? V : 0);
    break;
  case 9:
    z = ((o << 1) | (cc & C)) & 0xffu;
    cc = (cc & ~(N | Z | V | C)) | NZ8(z) | (o >> 7) |
         (((o ^ z) & 0x80u) ? V : 0);
    break;
  case 10:
    z = (o - 1u) & 0xffu;
    cc = (cc & ~(N | Z | V)) | NZ8(z) | (o == 0x80u ? V : 0);
    break;
  case 12:
    z = (o + 1u) & 0xffu;
    cc = (cc & ~(N | Z | V)) | NZ8(z) | (o == 0x7fu ? V : 0);
    break;
  case 13:
    cc = (cc & ~(N | Z | V)) | NZ8(o);
    break;
  case 15:
    z = 0;
    cc = (cc & ~(N | V | C)) | Z;
    break;
  default:
    c->illegal++;
    break;
  }
  c->cc = (uint8_t)cc;
  return z;
}

static inline int condition(uint32_t cc, uint32_t op) {
  const uint32_t z = (cc >> 2) & 1u, n = (cc >> 3) & 1u, vf = (cc >> 1) & 1u,
                 carry = cc & 1u;
  switch (op & 15u) {
  case 0:
    return 1;
  case 1:
    return 0;
  case 2:
    return !(carry | z);
  case 3:
    return (int)(carry | z);
  case 4:
    return !carry;
  case 5:
    return (int)carry;
  case 6:
    return !z;
  case 7:
    return (int)z;
  case 8:
    return !vf;
  case 9:
    return (int)vf;
  case 10:
    return !n;
  case 11:
    return (int)n;
  case 12:
    return n == vf;
  case 13:
    return n != vf;
  case 14:
    return !z && n == vf;
  default:
    return z || n != vf;
  }
}

/* the bytes a push or a pull moves: one cycle each */
static inline uint32_t stack_cycles(uint32_t mask) {
  static const uint8_t bytes[16] = {0, 1, 1, 2, 1, 2, 2, 3,
                                    1, 2, 2, 3, 2, 3, 3, 4};
  return bytes[mask & 15u] + 2u * bytes[mask >> 4];
}

extern const uint8_t m6809_cycles0[256];
void vx_fast_boot_hook(Vectrex *v);

#ifdef VX_COVERAGE
/* the host's --coverage: instructions started per address (the run loop
 * sees them all under --interp), the recompiler's profile, and the
 * distinct addresses played, the explorer's novelty (m0_explore.h) */
uint32_t vx_coverage_hits[65536];
uint32_t vx_coverage_locations;
#define COVER(pc) do { if (!vx_coverage_hits[(pc)]++) vx_coverage_locations++; } while (0)
#else
#define COVER(pc) ((void)0)
#endif

#ifdef VX_NATIVE
#define VX_LOOP_FN static void m6809_loop
#else
#define VX_LOOP_FN void m6809_run
#endif
VX_LOOP_FN(Vectrex *v, uint32_t stop) {
  M6809 *const c = &v->cpu;
  const uint8_t *const *const rmap = v->rmap;
  uint8_t *const *const wmap = v->wmap;
  uint32_t pc = c->pc, cycles = c->cycles, start = c->start;
  while ((int32_t)(cycles - stop) < 0) {
    start = cycles;
    COVER(pc);
    if (v->attention) {
      v->attention = (uint8_t)((v->via.ier & 0x7f) | c->waiting | v->fast_boot |
                               (c->illegal >= 1000));
      if (c->illegal >= 1000)
        break;
      if (v->fast_boot) {
        SAVE();
        vx_fast_boot_hook(v);
        pc = c->pc;
      }
      if ((v->via.ier & 0x7f) | c->waiting)
        goto slow;
    }
    const uint8_t *const page = rmap[pc >> 10];
    /* code outside plain memory: an opcode read may have side effects
     * there, the reference reads it once */
    if (!page) {
    slow:
      SAVE();
      m6809_step(v);
      pc = c->pc;
      cycles = c->cycles;
      start = c->start;
      v->attention = (uint8_t)((v->via.ier & 0x7f) | c->waiting | v->fast_boot |
                               (c->illegal >= 1000));
      continue;
    }
    const uint32_t op = page[pc & 1023u];
    pc = (pc + 1u) & 0xffffu;
    cycles += m6809_cycles0[op];
    uint32_t ad, mode;
    /* the unary groups' holes (x1, x2, x5, xB): the reference counts them */
    if ((0x00f1u >> (op >> 4) & 1u) && ((0x0826u >> (op & 15u)) & 1u))
      goto undo;
    switch (op >> 4) {
    case 0x0:
      ad = (uint32_t)c->dp << 8 | FETCH();
      goto memory_unary;
    case 0x7:
      ad = FETCH16();
      goto memory_unary;
    case 0x2:
      if (condition(c->cc, op)) {
        const uint32_t off = (uint32_t)(int8_t)FETCH();
        pc = (pc + off) & 0xffffu;
      } else
        pc = (pc + 1u) & 0xffffu;
      continue;
    case 0x4:
      if (op == 0x4e)
        goto undo;
      c->a = (uint8_t)unary(c, op & 15u, c->a);
      continue;
    case 0x5:
      if (op == 0x5e)
        goto undo;
      c->b = (uint8_t)unary(c, op & 15u, c->b);
      continue;
    case 0x6:
      mode = 2;
      goto address;
    case 0x1:
      switch (op) {
      case 0x12:
        continue;
      case 0x16: {
        const uint32_t off = FETCH16();
        pc = (pc + off) & 0xffffu;
        continue;
      }
      case 0x17: {
        const uint32_t off = FETCH16();
        PUSH16(s, pc);
        pc = (pc + off) & 0xffffu;
        continue;
      }
      case 0x19: {
        const uint32_t a = c->a;
        uint32_t fix = 0;
        if ((a & 15u) > 9u || (c->cc & H))
          fix |= 0x06u;
        if (a > 0x99u || (c->cc & C))
          fix |= 0x60u;
        const uint32_t r = (a + fix) & 0xffu;
        c->a = (uint8_t)r;
        SETNZ8(r);
        if (fix & 0x60u)
          c->cc |= C;
        continue;
      }
      case 0x1a:
        c->cc |= (uint8_t)FETCH();
        continue;
      case 0x1c:
        c->cc &= (uint8_t)FETCH();
        continue;
      case 0x1d:
        c->a = (c->b & 0x80u) ? 0xff : 0;
        SETNZ16(D());
        continue;
      }
      goto undo;
    case 0x3:
      switch (op) {
      case 0x30:
      case 0x31:
      case 0x32:
      case 0x33:
        mode = 2;
        goto address;
      case 0x34:
      case 0x36: {
        const uint32_t mask = FETCH();
        cycles += stack_cycles(mask);
        if (op == 0x34) {
          if (mask & 0x80u)
            PUSH16(s, pc);
          if (mask & 0x40u)
            PUSH16(s, c->u);
          if (mask & 0x20u)
            PUSH16(s, c->y);
          if (mask & 0x10u)
            PUSH16(s, c->x);
          if (mask & 8u)
            PUSH8(s, c->dp);
          if (mask & 4u)
            PUSH8(s, c->b);
          if (mask & 2u)
            PUSH8(s, c->a);
          if (mask & 1u)
            PUSH8(s, c->cc);
        } else {
          if (mask & 0x80u)
            PUSH16(u, pc);
          if (mask & 0x40u)
            PUSH16(u, c->s);
          if (mask & 0x20u)
            PUSH16(u, c->y);
          if (mask & 0x10u)
            PUSH16(u, c->x);
          if (mask & 8u)
            PUSH8(u, c->dp);
          if (mask & 4u)
            PUSH8(u, c->b);
          if (mask & 2u)
            PUSH8(u, c->a);
          if (mask & 1u)
            PUSH8(u, c->cc);
        }
        continue;
      }
      case 0x35:
      case 0x37: {
        const uint32_t mask = FETCH();
        cycles += stack_cycles(mask);
        if (op == 0x35) {
          if (mask & 1u)
            c->cc = (uint8_t)PULL8(s);
          if (mask & 2u)
            c->a = (uint8_t)PULL8(s);
          if (mask & 4u)
            c->b = (uint8_t)PULL8(s);
          if (mask & 8u)
            c->dp = (uint8_t)PULL8(s);
          if (mask & 0x10u)
            c->x = (uint16_t)PULL16(s);
          if (mask & 0x20u)
            c->y = (uint16_t)PULL16(s);
          if (mask & 0x40u)
            c->u = (uint16_t)PULL16(s);
          if (mask & 0x80u)
            pc = PULL16(s);
        } else {
          if (mask & 1u)
            c->cc = (uint8_t)PULL8(u);
          if (mask & 2u)
            c->a = (uint8_t)PULL8(u);
          if (mask & 4u)
            c->b = (uint8_t)PULL8(u);
          if (mask & 8u)
            c->dp = (uint8_t)PULL8(u);
          if (mask & 0x10u)
            c->x = (uint16_t)PULL16(u);
          if (mask & 0x20u)
            c->y = (uint16_t)PULL16(u);
          if (mask & 0x40u)
            c->s = (uint16_t)PULL16(u);
          if (mask & 0x80u)
            pc = PULL16(u);
        }
        continue;
      }
      case 0x39:
        pc = PULL16(s);
        continue;
      case 0x3a:
        c->x = (uint16_t)(c->x + c->b);
        continue;
      case 0x3d: {
        const uint32_t m = (uint32_t)c->a * c->b;
        SETD(m);
        c->cc = (uint8_t)((c->cc & ~(Z | C)) | (m ? 0 : Z) | ((m >> 7) & C));
        continue;
      }
      }
      goto undo;
    default:
      mode = (op >> 4) & 3u;
      goto address;
    }

  address:
    /* the effective address: the memory group (0x80-0xFF), the indexed
     * unary ops (0x6x), LEA (0x30-0x33) */
    ad = 0;
    if (mode == 1)
      ad = (uint32_t)c->dp << 8 | FETCH();
    else if (mode == 3)
      ad = FETCH16();
    else if (mode == 2) {
      const uint32_t pb = FETCH();
      uint16_t *const r = &c->x + ((pb >> 5) & 3u);
      if (!(pb & 0x80u)) {
        cycles += 1;
        ad = (*r + ((pb & 31u) ^ 16u) - 16u) & 0xffffu;
      } else {
        ad = *r;
        switch (pb & 15u) {
        case 0:
          *r = (uint16_t)(*r + 1u);
          cycles += 2;
          break;
        case 1:
          *r = (uint16_t)(*r + 2u);
          cycles += 3;
          break;
        case 2:
          ad = *r = (uint16_t)(*r - 1u);
          cycles += 2;
          break;
        case 3:
          ad = *r = (uint16_t)(*r - 2u);
          cycles += 3;
          break;
        case 4:
          break;
        case 5:
          ad = (ad + (uint32_t)(int8_t)c->b) & 0xffffu;
          cycles += 1;
          break;
        case 6:
          ad = (ad + (uint32_t)(int8_t)c->a) & 0xffffu;
          cycles += 1;
          break;
        case 8: {
          const uint32_t o = (uint32_t)(int8_t)FETCH();
          ad = (ad + o) & 0xffffu;
          cycles += 1;
          break;
        }
        case 9: {
          const uint32_t o = FETCH16();
          ad = (ad + o) & 0xffffu;
          cycles += 4;
          break;
        }
        case 11:
          ad = (ad + D()) & 0xffffu;
          cycles += 4;
          break;
        case 12: {
          const uint32_t o = (uint32_t)(int8_t)FETCH();
          ad = (pc + o) & 0xffffu;
          cycles += 1;
          break;
        }
        case 13: {
          const uint32_t o = FETCH16();
          ad = (pc + o) & 0xffffu;
          cycles += 5;
          break;
        }
        case 15:
          ad = FETCH16();
          cycles += 2;
          break;
        default:
          c->illegal++;
          v->attention = 1;
          break;
        }
        if (pb & 0x10u) {
          ad = RD16(ad);
          cycles += 3;
        }
      }
    }
    if (op < 0x80) {
      if (op >= 0x60)
        goto memory_unary;
      /* LEA: X and Y set Z, S and U leave the flags */
      if (op == 0x30) {
        c->x = (uint16_t)ad;
        c->cc = (uint8_t)((c->cc & ~Z) | (ad ? 0 : Z));
      } else if (op == 0x31) {
        c->y = (uint16_t)ad;
        c->cc = (uint8_t)((c->cc & ~Z) | (ad ? 0 : Z));
      } else if (op == 0x32)
        c->s = (uint16_t)ad;
      else
        c->u = (uint16_t)ad;
      continue;
    }
    {
      const uint32_t lo = op & 15u, isb = op >= 0xc0;
      uint8_t *const r = isb ? &c->b : &c->a;
      uint32_t val;
      switch (lo) {
      case 3:
        val = mode ? RD16(ad) : FETCH16();
        SETD(isb ? add16(c, D(), val) : sub16(c, D(), val));
        continue;
      case 12:
        val = mode ? RD16(ad) : FETCH16();
        if (isb) {
          SETD(val);
          SETNZ16(val);
        } else
          sub16(c, c->x, val);
        continue;
      case 14:
        val = mode ? RD16(ad) : FETCH16();
        if (isb)
          c->u = (uint16_t)val;
        else
          c->x = (uint16_t)val;
        SETNZ16(val);
        continue;
      case 15:
        if (!mode) {
          c->illegal++;
          v->attention = 1;
          continue;
        }
        val = isb ? c->u : c->x;
        WR16(ad, val);
        SETNZ16(val);
        continue;
      case 13:
        if (!isb) {
          if (!mode) {
            const uint32_t off = (uint32_t)(int8_t)FETCH();
            PUSH16(s, pc);
            pc = (pc + off) & 0xffffu;
          } else {
            PUSH16(s, pc);
            pc = ad;
          }
        } else if (!mode) {
          c->illegal++;
          v->attention = 1;
        } else {
          val = D();
          WR16(ad, val);
          SETNZ16(val);
        }
        continue;
      case 7:
        if (!mode) {
          c->illegal++;
          v->attention = 1;
          continue;
        }
        WR(ad, *r);
        SETNZ8(*r);
        continue;
      }
      const uint32_t t = mode ? RD(ad) : FETCH();
      switch (lo) {
      case 0:
        *r = (uint8_t)sub8(c, *r, t, 0);
        break;
      case 1:
        sub8(c, *r, t, 0);
        break;
      case 2:
        *r = (uint8_t)sub8(c, *r, t, c->cc & C);
        break;
      case 4:
        *r &= (uint8_t)t;
        SETNZ8(*r);
        break;
      case 5:
        SETNZ8(*r & t);
        break;
      case 6:
        *r = (uint8_t)t;
        SETNZ8(t);
        break;
      case 8:
        *r ^= (uint8_t)t;
        SETNZ8(*r);
        break;
      case 9:
        *r = (uint8_t)add8(c, *r, t, c->cc & C);
        break;
      case 10:
        *r |= (uint8_t)t;
        SETNZ8(*r);
        break;
      case 11:
        *r = (uint8_t)add8(c, *r, t, 0);
        break;
      }
      continue;
    }

  memory_unary: {
    const uint32_t lo = op & 15u;
    if (lo == 14) {
      pc = ad;
      continue;
    }
    const uint32_t t = RD(ad);
    const uint32_t z = unary(c, lo, t);
    if (lo != 13)
      WR(ad, z);
    continue;
  }

  undo:
    /* back to the opcode: the reference runs the whole instruction */
    pc = (pc - 1u) & 0xffffu;
    cycles -= m6809_cycles0[op];
    goto slow;
  }
  SAVE();
}

#ifdef VX_NATIVE
/* Recompiled code (vx_gen.c) in front of the run loop.  A region runs
 * while nothing needs a closer look (attention clear: no enabled VIA
 * interrupt, no wait, no boot shortcut) until the slice ends (precisely:
 * after the instruction that spends it, the run loop's boundary; otherwise
 * at its next entry) or until a transfer it cannot follow.  Anything else
 * is the run loop's, one instruction at a time.  vx_native = 0 is the
 * reference (--interp). */
#include "vx_gen.h"

int vx_native = 1;
#ifdef VX_GEN_STATS
uint32_t vx_gen_calls, vx_gen_steps; /* the host prints them */
#define GEN_STAT(n) ((n)++)
#else
#define GEN_STAT(n) ((void)0)
#endif

uint32_t vx_bios_sum(const uint8_t *bios) {
  uint32_t h = 2166136261u;
  for (unsigned i = 0; i < 8192; i++)
    h = (h ^ bios[i]) * 16777619u;
  return h;
}

#include "vx_via_asm.h"
#if !VX_VIA_ASM /* via_asm.S has them otherwise */
uint32_t vxg_read(Vectrex *v, uint32_t addr, uint32_t start, uint32_t info) {
  M6809 *const c = &v->cpu;
  if (info >> 8)
    c->pc = (uint16_t)(info >> 8);
  c->start = start;
  c->cycles = start + (info & 0xffu);
  /* the VIA without vx_read's dispatch: nearly every slow read is its */
  if ((addr & 0xf800u) == 0xd000u)
    return via_read(v, addr & 15u);
  return vx_read(v, (uint16_t)addr);
}
uint32_t vxg_write(Vectrex *v, uint32_t addr_data, uint32_t start,
                   uint32_t counted) {
  M6809 *const c = &v->cpu;
  c->start = start;
  c->cycles = start + counted;
  if ((addr_data & 0xf800u) == 0xd000u)
    via_write(v, addr_data & 15u, (uint8_t)(addr_data >> 16));
  else
    vx_write(v, (uint16_t)addr_data, (uint8_t)(addr_data >> 16));
  return v->attention;
}
#endif

/* the run loop's pushes and pulls (m6809_run's PUSH8/PULL8 order), the
 * clock as the recompiled code holds it */
uint32_t vxg_push(Vectrex *v, uint32_t op, uint32_t pc, uint32_t start,
                  uint32_t counted) {
  M6809 *const c = &v->cpu;
  uint16_t *const sp = (op & 0x100u) ? &c->u : &c->s;
  const uint32_t other = (op & 0x100u) ? c->s : c->u;
  c->start = start;
  c->cycles = start + counted;
#define PUSH(x)                                                                \
  do {                                                                         \
    *sp = (uint16_t)(*sp - 1u);                                                \
    vx_write(v, *sp, (uint8_t)(x));                                            \
  } while (0)
  if (op & 0x80u) {
    PUSH(pc);
    PUSH(pc >> 8);
  }
  if (op & 0x40u) {
    PUSH(other);
    PUSH(other >> 8);
  }
  if (op & 0x20u) {
    PUSH(c->y);
    PUSH(c->y >> 8);
  }
  if (op & 0x10u) {
    PUSH(c->x);
    PUSH(c->x >> 8);
  }
  if (op & 8u)
    PUSH(c->dp);
  if (op & 4u)
    PUSH(c->b);
  if (op & 2u)
    PUSH(c->a);
  if (op & 1u)
    PUSH(c->cc);
#undef PUSH
  return v->attention;
}
uint32_t vxg_pull(Vectrex *v, uint32_t op, uint32_t start, uint32_t info) {
  M6809 *const c = &v->cpu;
  uint16_t *const sp = (op & 0x100u) ? &c->u : &c->s;
  uint32_t pc = 0;
  c->pc = (uint16_t)(info >> 8);
  c->start = start;
  c->cycles = start + (info & 0xffu);
  /* the pointer moves first: a VIA read sees it moved */
#define PULL() (*sp = (uint16_t)(*sp + 1u), vx_read(v, (uint16_t)(*sp - 1u)))
  if (op & 1u)
    c->cc = PULL();
  if (op & 2u)
    c->a = PULL();
  if (op & 4u)
    c->b = PULL();
  if (op & 8u)
    c->dp = PULL();
  if (op & 0x10u) {
    const uint32_t h = PULL();
    c->x = (uint16_t)(h << 8 | PULL());
  }
  if (op & 0x20u) {
    const uint32_t h = PULL();
    c->y = (uint16_t)(h << 8 | PULL());
  }
  if (op & 0x40u) {
    const uint32_t h = PULL();
    const uint32_t x = h << 8 | PULL();
    if (op & 0x100u)
      c->s = (uint16_t)x;
    else
      c->u = (uint16_t)x;
  }
  if (op & 0x80u) {
    const uint32_t h = PULL();
    pc = h << 8 | PULL();
  }
#undef PULL
  return pc;
}

void m6809_run(Vectrex *v, uint32_t stop) {
  M6809 *const c = &v->cpu;
  if (!vx_native) {
    m6809_loop(v, stop);
    return;
  }
  /* where the recompiled code last could not follow a transfer (RTS, PULS
   * PC, an indirect jump): the same sites come back, one entry of a small
   * direct-mapped cache spares the search */
  typedef struct { uint16_t pc; int16_t r; } GenBack;
  static GenBack back[8];
  int hint = -1;
  while ((int32_t)(c->cycles - stop) < 0) {
    if (!v->attention) {
      const uint32_t pc = c->pc;
      int r = hint;
      if (r < 0 || pc < vx_gen_first[r] || pc > vx_gen_last[r]) {
        GenBack *const e = &back[(pc >> 2) & 7u];
        r = e->pc == pc ? e->r : vx_gen_find(pc);
        e->pc = (uint16_t)pc;
        e->r = (int16_t)r;
      }
      if (r >= 0) {
        GEN_STAT(vx_gen_calls);
        hint = vx_gen_fn[r](v, stop, pc - vx_gen_first[r]);
        if (hint != -2)
          continue;
      }
    } else if (c->illegal >= 1000)
      break;
    GEN_STAT(vx_gen_steps);
    hint = -1;
    m6809_loop(v, c->cycles + 1u);
  }
}
#endif
