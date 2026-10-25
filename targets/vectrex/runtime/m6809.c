/* MC6809 interpreter with the data sheet's cycle counts (the Vectrex times
 * many strokes with instructions: the beam moves while they run). */
#include "vectrex.h"
#include <stdint.h>
#define C 1
#define V 2
#define Z 4
#define N 8
#define I 16
#define H 32
#define F 64
#define E 128
static inline uint8_t rb(Vectrex *v, uint16_t a) {
  if (a < 0x8000)
    return a < vectrex_cart_size ? vectrex_cart[a] : 0xff;
  if (a >= 0xe000)
    return v->bios[a - 0xe000];
  if ((a & 0xf800) == 0xc800)
    return v->ram[a & 1023];
  return vx_read(v, a);
}
static inline void wb(Vectrex *v, uint16_t a, uint8_t x) {
  v->cpu.writes++;
  if ((a & 0xf800) == 0xc800)
    v->ram[a & 1023] = x;
  else
    vx_write(v, a, x);
}
static uint16_t rw(Vectrex *v, uint16_t a) {
  return (uint16_t)(rb(v, a) << 8 | rb(v, (uint16_t)(a + 1)));
}
static void ww(Vectrex *v, uint16_t a, uint16_t x) {
  wb(v, a, x >> 8);
  wb(v, (uint16_t)(a + 1), (uint8_t)x);
}
static inline uint8_t fetch(Vectrex *v) { return rb(v, v->cpu.pc++); }
static uint16_t fetch16(Vectrex *v) {
  uint16_t a = fetch(v);
  return (uint16_t)(a << 8 | fetch(v));
}
static inline uint16_t d(M6809 *c) { return (uint16_t)(c->a << 8 | c->b); }
static inline void sd(M6809 *c, uint16_t x) {
  c->a = x >> 8;
  c->b = (uint8_t)x;
}
static inline void nz8(M6809 *c, uint8_t x) {
  c->cc = (c->cc & ~(N | Z | V)) | (x & 0x80 ? N : 0) | (x ? 0 : Z);
}
static inline void nz16(M6809 *c, uint16_t x) {
  c->cc = (c->cc & ~(N | Z | V)) | (x & 0x8000 ? N : 0) | (x ? 0 : Z);
}
static uint8_t add8(M6809 *c, uint8_t a, uint8_t b, int carry) {
  unsigned r = a + b + carry;
  uint8_t z = (uint8_t)r;
  c->cc = (c->cc & ~(N | Z | V | C | H)) | (z & 0x80 ? N : 0) | (z ? 0 : Z) |
          (((~(a ^ b)) & (a ^ z) & 0x80) ? V : 0) | (r > 255 ? C : 0) |
          (((a & 15) + (b & 15) + carry) > 15 ? H : 0);
  return z;
}
static uint8_t sub8(M6809 *c, uint8_t a, uint8_t b, int carry) {
  unsigned r = a - b - carry;
  uint8_t z = (uint8_t)r;
  c->cc = (c->cc & ~(N | Z | V | C)) | (z & 0x80 ? N : 0) | (z ? 0 : Z) |
          (((a ^ b) & (a ^ z) & 0x80) ? V : 0) | (r > 255 ? C : 0);
  return z;
}
static uint16_t sub16(M6809 *c, uint16_t a, uint16_t b) {
  unsigned r = a - b;
  uint16_t z = (uint16_t)r;
  c->cc = (c->cc & ~(N | Z | V | C)) | (z & 0x8000 ? N : 0) | (z ? 0 : Z) |
          (((a ^ b) & (a ^ z) & 0x8000) ? V : 0) | (r > 65535 ? C : 0);
  return z;
}
static uint16_t add16(M6809 *c, uint16_t a, uint16_t b) {
  unsigned r = a + b;
  uint16_t z = (uint16_t)r;
  c->cc = (c->cc & ~(N | Z | V | C)) | (z & 0x8000 ? N : 0) | (z ? 0 : Z) |
          ((~(a ^ b) & (a ^ z) & 0x8000) ? V : 0) | (r > 65535 ? C : 0);
  return z;
}
static void push8(Vectrex *v, uint16_t *sp, uint8_t x) { wb(v, --*sp, x); }
static void push16(Vectrex *v, uint16_t *sp, uint16_t x) {
  push8(v, sp, (uint8_t)x);
  push8(v, sp, x >> 8);
}
static uint8_t pull8(Vectrex *v, uint16_t *sp) { return rb(v, (*sp)++); }
static uint16_t pull16(Vectrex *v, uint16_t *sp) {
  uint16_t a = pull8(v, sp);
  return (uint16_t)(a << 8 | pull8(v, sp));
}

/* cycles of the unprefixed opcodes, the indexed modes' extra excluded */
const uint8_t m6809_cycles0[256] = {
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6, /* 0x */
    0, 0, 2, 4, 2, 2, 5, 9, 2, 2, 3, 2, 3, 2, 8, 6, /* 1x */
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, /* 2x */
    4, 4, 4, 4, 5, 5, 5, 5, 2, 5, 3, 6, 20, 11, 2, 19, /* 3x */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, /* 4x */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, /* 5x */
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6, /* 6x */
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 4, 7, /* 7x */
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 4, 7, 3, 2, /* 8x */
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5, /* 9x */
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5, /* Ax */
    5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 7, 8, 6, 6, /* Bx */
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 3, 2, /* Cx */
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, /* Dx */
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, /* Ex */
    5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, /* Fx */
};

static uint16_t indexed(Vectrex *v) {
  M6809 *c = &v->cpu;
  uint8_t p = fetch(v);
  uint16_t *r = (p & 0x60) == 0x00   ? &c->x
                : (p & 0x60) == 0x20 ? &c->y
                : (p & 0x60) == 0x40 ? &c->u
                                     : &c->s;
  if (!(p & 0x80)) {
    c->cycles += 1;
    return (uint16_t)(*r + ((p & 31) ^ 16) - 16);
  }
  uint16_t a = *r;
  switch (p & 15) {
  case 0:
    *r += 1;
    c->cycles += 2;
    break;
  case 1:
    *r += 2;
    c->cycles += 3;
    break;
  case 2:
    a = --*r;
    c->cycles += 2;
    break;
  case 3:
    a = (*r -= 2);
    c->cycles += 3;
    break;
  case 4:
    break;
  case 5:
    a += (int8_t)c->b;
    c->cycles += 1;
    break;
  case 6:
    a += (int8_t)c->a;
    c->cycles += 1;
    break;
  case 8:
    a += (int8_t)fetch(v);
    c->cycles += 1;
    break;
  case 9:
    a += fetch16(v);
    c->cycles += 4;
    break;
  case 11:
    a += d(c);
    c->cycles += 4;
    break;
  case 12: {
    int8_t offset = (int8_t)fetch(v);
    a = (uint16_t)(c->pc + offset);
    c->cycles += 1;
    break;
  }
  case 13: {
    uint16_t offset = fetch16(v);
    a = (uint16_t)(c->pc + offset);
    c->cycles += 5;
    break;
  }
  case 15:
    a = fetch16(v);
    c->cycles += 2;
    break;
  default:
    c->illegal++;
    break;
  }
  if (p & 0x10) {
    a = rw(v, a);
    c->cycles += 3;
  }
  return a;
}
/* 1 direct, 2 indexed, 3 extended */
static uint16_t ea(Vectrex *v, int mode) {
  M6809 *c = &v->cpu;
  if (mode == 1)
    return (uint16_t)((c->dp << 8) | fetch(v));
  if (mode == 2)
    return indexed(v);
  return fetch16(v);
}
static int condition(M6809 *c, int n) {
  int z = !!(c->cc & Z), nflag = !!(c->cc & N), v = !!(c->cc & V),
      carry = !!(c->cc & C);
  switch (n & 15) {
  case 0:
    return 1;
  case 1:
    return 0;
  case 2:
    return !carry && !z;
  case 3:
    return carry || z;
  case 4:
    return !carry;
  case 5:
    return carry;
  case 6:
    return !z;
  case 7:
    return z;
  case 8:
    return !v;
  case 9:
    return v;
  case 10:
    return !nflag;
  case 11:
    return nflag;
  case 12:
    return nflag == v;
  case 13:
    return nflag != v;
  case 14:
    return !z && (nflag == v);
  default:
    return z || (nflag != v);
  }
}
static uint16_t getreg(M6809 *c, int r) {
  switch (r) {
  case 0:
    return d(c);
  case 1:
    return c->x;
  case 2:
    return c->y;
  case 3:
    return c->u;
  case 4:
    return c->s;
  case 5:
    return c->pc;
  case 8:
    return 0xff00 | c->a;
  case 9:
    return 0xff00 | c->b;
  case 10:
    return 0xff00 | c->cc;
  case 11:
    return 0xff00 | c->dp;
  default:
    return 0xffff;
  }
}
static void setreg(M6809 *c, int r, uint16_t z) {
  switch (r) {
  case 0:
    sd(c, z);
    break;
  case 1:
    c->x = z;
    break;
  case 2:
    c->y = z;
    break;
  case 3:
    c->u = z;
    break;
  case 4:
    c->s = z;
    break;
  case 5:
    c->pc = z;
    break;
  case 8:
    c->a = (uint8_t)z;
    break;
  case 9:
    c->b = (uint8_t)z;
    break;
  case 10:
    c->cc = (uint8_t)z;
    break;
  case 11:
    c->dp = (uint8_t)z;
    break;
  }
}
static void stackop(Vectrex *v, int op) {
  M6809 *c = &v->cpu;
  int use_s = !(op & 2);
  uint16_t *sp = use_s ? &c->s : &c->u;
  uint16_t other = use_s ? c->u : c->s;
  uint8_t mask = fetch(v);
  /* one cycle a byte */
  c->cycles += (mask & 1) + (mask >> 1 & 1) + (mask >> 2 & 1) +
               (mask >> 3 & 1) + 2 * ((mask >> 4 & 1) + (mask >> 5 & 1) +
                                      (mask >> 6 & 1) + (mask >> 7 & 1));
  if (!(op & 1)) {
    if (mask & 0x80)
      push16(v, sp, c->pc);
    if (mask & 0x40)
      push16(v, sp, other);
    if (mask & 0x20)
      push16(v, sp, c->y);
    if (mask & 0x10)
      push16(v, sp, c->x);
    if (mask & 8)
      push8(v, sp, c->dp);
    if (mask & 4)
      push8(v, sp, c->b);
    if (mask & 2)
      push8(v, sp, c->a);
    if (mask & 1)
      push8(v, sp, c->cc);
  } else {
    if (mask & 1)
      c->cc = pull8(v, sp);
    if (mask & 2)
      c->a = pull8(v, sp);
    if (mask & 4)
      c->b = pull8(v, sp);
    if (mask & 8)
      c->dp = pull8(v, sp);
    if (mask & 0x10)
      c->x = pull16(v, sp);
    if (mask & 0x20)
      c->y = pull16(v, sp);
    if (mask & 0x40) {
      uint16_t x = pull16(v, sp);
      if (use_s)
        c->u = x;
      else
        c->s = x;
    }
    if (mask & 0x80)
      c->pc = pull16(v, sp);
  }
}
/* NEG .. CLR on a byte (the low nibble of 0x00-0x0F, 0x40-0x7F) */
static uint8_t unary(M6809 *c, int op, uint8_t a) {
  uint8_t z = a;
  switch (op & 15) {
  case 0:
    z = sub8(c, 0, a, 0);
    break;
  case 3:
    z = (uint8_t)~a;
    nz8(c, z);
    c->cc |= C;
    break;
  case 4: /* LSR: V unaffected */
    z = a >> 1;
    c->cc = (c->cc & ~(N | Z | C)) | (z ? 0 : Z) | (a & 1);
    break;
  case 6: { /* ROR */
    z = (uint8_t)((a >> 1) | ((c->cc & C) ? 0x80 : 0));
    c->cc = (c->cc & ~(N | Z | C)) | (z & 0x80 ? N : 0) | (z ? 0 : Z) | (a & 1);
    break;
  }
  case 7: /* ASR */
    z = (uint8_t)((a >> 1) | (a & 0x80));
    c->cc = (c->cc & ~(N | Z | C)) | (z & 0x80 ? N : 0) | (z ? 0 : Z) | (a & 1);
    break;
  case 8: /* ASL */
    z = (uint8_t)(a << 1);
    c->cc = (c->cc & ~(N | Z | V | C)) | (z & 0x80 ? N : 0) | (z ? 0 : Z) |
            (a & 0x80 ? C : 0) | (((a ^ z) & 0x80) ? V : 0);
    break;
  case 9: /* ROL */
    z = (uint8_t)((a << 1) | (c->cc & C));
    c->cc = (c->cc & ~(N | Z | V | C)) | (z & 0x80 ? N : 0) | (z ? 0 : Z) |
            (a & 0x80 ? C : 0) | (((a ^ z) & 0x80) ? V : 0);
    break;
  case 10:
    z = a - 1;
    nz8(c, z);
    c->cc |= a == 0x80 ? V : 0;
    break;
  case 12:
    z = a + 1;
    nz8(c, z);
    c->cc |= a == 0x7f ? V : 0;
    break;
  case 13:
    nz8(c, a);
    break;
  case 15:
    z = 0;
    c->cc = (c->cc & ~(N | V | C)) | Z;
    break;
  default:
    c->illegal++;
    break;
  }
  return z;
}
static void stack_all(Vectrex *v) {
  M6809 *c = &v->cpu;
  c->cc |= E;
  push16(v, &c->s, c->pc);
  push16(v, &c->s, c->u);
  push16(v, &c->s, c->y);
  push16(v, &c->s, c->x);
  push8(v, &c->s, c->dp);
  push8(v, &c->s, c->b);
  push8(v, &c->s, c->a);
  push8(v, &c->s, c->cc);
}
static void interrupt(Vectrex *v, uint16_t vector, uint8_t mask) {
  M6809 *c = &v->cpu;
  if (c->waiting != 1)
    stack_all(v);
  c->cc |= mask;
  c->pc = rw(v, vector);
  c->waiting = 0;
  c->cycles += 19;
}
/* 0x80-0xFF: the accumulators' and the 16-bit registers' memory group */
static void group(Vectrex *v, uint8_t op) {
  M6809 *c = &v->cpu;
  int mode = (op >> 4) & 3, lo = op & 15, b = op >= 0xc0;
  uint8_t *r = b ? &c->b : &c->a;
  uint16_t addr = mode ? ea(v, mode) : 0, val;
  switch (lo) {
  case 3:
    val = mode ? rw(v, addr) : fetch16(v);
    sd(c, b ? add16(c, d(c), val) : sub16(c, d(c), val));
    return;
  case 12:
    val = mode ? rw(v, addr) : fetch16(v);
    if (b) {
      sd(c, val);
      nz16(c, val);
    } else
      sub16(c, c->x, val);
    return;
  case 14:
    val = mode ? rw(v, addr) : fetch16(v);
    if (b)
      c->u = val;
    else
      c->x = val;
    nz16(c, val);
    return;
  case 15:
    if (!mode) {
      c->illegal++;
      return;
    }
    val = b ? c->u : c->x;
    ww(v, addr, val);
    nz16(c, val);
    return;
  case 13:
    if (!b) {
      if (!mode) {
        int8_t off = (int8_t)fetch(v);
        push16(v, &c->s, c->pc);
        c->pc = (uint16_t)(c->pc + off);
      } else {
        push16(v, &c->s, c->pc);
        c->pc = addr;
      }
    } else if (!mode) {
      c->illegal++;
    } else {
      ww(v, addr, d(c));
      nz16(c, d(c));
    }
    return;
  case 7:
    if (!mode) {
      c->illegal++;
      return;
    }
    wb(v, addr, *r);
    nz8(c, *r);
    return;
  }
  uint8_t x = mode ? rb(v, addr) : fetch(v);
  switch (lo) {
  case 0:
    *r = sub8(c, *r, x, 0);
    break;
  case 1:
    sub8(c, *r, x, 0);
    break;
  case 2:
    *r = sub8(c, *r, x, c->cc & C);
    break;
  case 4:
    *r &= x;
    nz8(c, *r);
    break;
  case 5:
    nz8(c, *r & x);
    break;
  case 6:
    *r = x;
    nz8(c, *r);
    break;
  case 8:
    *r ^= x;
    nz8(c, *r);
    break;
  case 9:
    *r = add8(c, *r, x, c->cc & C);
    break;
  case 10:
    *r |= x;
    nz8(c, *r);
    break;
  case 11:
    *r = add8(c, *r, x, 0);
    break;
  }
}
/* the 0x10 and 0x11 pages */
static void prefixed(Vectrex *v, uint8_t page) {
  M6809 *c = &v->cpu;
  uint8_t op = fetch(v);
  if (page == 0x10 && op >= 0x21 && op <= 0x2f) {
    int16_t off = (int16_t)fetch16(v);
    c->cycles += 5;
    if (condition(c, op)) {
      c->pc = (uint16_t)(c->pc + off);
      c->cycles += 1;
    }
    return;
  }
  if (op == 0x3f) {
    stack_all(v);
    c->pc = rw(v, page == 0x10 ? 0xfff4 : 0xfff2);
    c->cycles += 20;
    return;
  }
  int mode = (op >> 4) & 3, lo = op & 15;
  static const uint8_t compare[4] = {5, 7, 7, 8}, load[4] = {4, 6, 6, 7};
  if (op < 0x80) {
    c->illegal++;
    c->cycles += 2;
    return;
  }
  if (page == 0x10 && op < 0xc0 && (lo == 3 || lo == 12)) {
    c->cycles += compare[mode];
    uint16_t addr = mode ? ea(v, mode) : 0;
    uint16_t val = mode ? rw(v, addr) : fetch16(v);
    sub16(c, lo == 3 ? d(c) : c->y, val);
  } else if (page == 0x11 && op < 0xc0 && (lo == 3 || lo == 12)) {
    c->cycles += compare[mode];
    uint16_t addr = mode ? ea(v, mode) : 0;
    uint16_t val = mode ? rw(v, addr) : fetch16(v);
    sub16(c, lo == 3 ? c->u : c->s, val);
  } else if (page == 0x10 && lo == 14) {
    c->cycles += load[mode];
    uint16_t addr = mode ? ea(v, mode) : 0;
    uint16_t val = mode ? rw(v, addr) : fetch16(v);
    if (op < 0xc0)
      c->y = val;
    else
      c->s = val;
    nz16(c, val);
  } else if (page == 0x10 && lo == 15 && mode) {
    c->cycles += load[mode];
    uint16_t addr = ea(v, mode);
    uint16_t val = op < 0xc0 ? c->y : c->s;
    ww(v, addr, val);
    nz16(c, val);
  } else {
    c->illegal++;
    c->cycles += 2;
  }
}
void m6809_step(Vectrex *v) {
  M6809 *c = &v->cpu;
  c->start = c->cycles;
  /* the VIA's interrupt: only enabled sources need it up to date */
  if (v->via.ier & 0x7f)
    via_sync(v, c->cycles);
  if (v->via.ifr & v->via.ier & 0x7f) {
    if (c->waiting == 2)
      c->waiting = 0;
    if (!(c->cc & I)) {
      interrupt(v, 0xfff8, I);
      return;
    }
  }
  if (c->waiting) {
    c->cycles += 2;
    return;
  }
  uint8_t op = fetch(v);
  if (op == 0x10 || op == 0x11) {
    prefixed(v, op);
    return;
  }
  c->cycles += m6809_cycles0[op];
  if (op >= 0x80) {
    group(v, op);
    return;
  }
  switch (op >> 4) {
  case 0x0:
  case 0x6:
  case 0x7: {
    int lo = op & 15;
    uint16_t addr = ea(v, op < 0x10 ? 1 : op < 0x70 ? 2 : 3);
    if (lo == 14) {
      c->pc = addr;
      return;
    }
    uint8_t a = rb(v, addr);
    if (lo == 13) {
      nz8(c, a);
      return;
    }
    wb(v, addr, unary(c, lo, a));
    return;
  }
  case 0x2:
    if (condition(c, op)) {
      int8_t off = (int8_t)fetch(v);
      c->pc = (uint16_t)(c->pc + off);
    } else
      c->pc++;
    return;
  case 0x4:
  case 0x5: {
    uint8_t *r = (op & 0x10) ? &c->b : &c->a;
    if ((op & 15) == 14) {
      c->illegal++;
      return;
    }
    *r = unary(c, op, *r);
    return;
  }
  }
  switch (op) {
  case 0x12:
    break;
  case 0x13:
    c->waiting = 2;
    break;
  case 0x16: {
    int16_t x = (int16_t)fetch16(v);
    c->pc = (uint16_t)(c->pc + x);
    break;
  }
  case 0x17: {
    int16_t x = (int16_t)fetch16(v);
    push16(v, &c->s, c->pc);
    c->pc = (uint16_t)(c->pc + x);
    break;
  }
  case 0x19: {
    uint8_t a = c->a, fix = 0;
    if ((a & 15) > 9 || (c->cc & H))
      fix |= 0x06;
    if (a > 0x99 || (c->cc & C))
      fix |= 0x60;
    unsigned r = a + fix;
    c->a = (uint8_t)r;
    nz8(c, c->a);
    if (fix & 0x60)
      c->cc |= C;
    break;
  }
  case 0x1a:
    c->cc |= fetch(v);
    break;
  case 0x1c:
    c->cc &= fetch(v);
    break;
  case 0x1d:
    c->a = (c->b & 0x80) ? 0xff : 0;
    nz16(c, d(c));
    break;
  case 0x1e:
  case 0x1f: {
    uint8_t x = fetch(v);
    uint16_t a = getreg(c, x >> 4), b = getreg(c, x & 15);
    if (op == 0x1e)
      setreg(c, x >> 4, b);
    setreg(c, x & 15, a);
    break;
  }
  case 0x30:
  case 0x31:
  case 0x32:
  case 0x33: {
    uint16_t a = indexed(v);
    if (op == 0x30)
      c->x = a;
    else if (op == 0x31)
      c->y = a;
    else if (op == 0x32)
      c->s = a;
    else
      c->u = a;
    if (op < 0x32)
      c->cc = (c->cc & ~Z) | (a ? 0 : Z);
    break;
  }
  case 0x34:
  case 0x35:
  case 0x36:
  case 0x37:
    stackop(v, op);
    break;
  case 0x39:
    c->pc = pull16(v, &c->s);
    break;
  case 0x3a:
    c->x = (uint16_t)(c->x + c->b);
    break;
  case 0x3b: {
    uint8_t cc = pull8(v, &c->s);
    c->cc = cc;
    if (cc & E) {
      c->a = pull8(v, &c->s);
      c->b = pull8(v, &c->s);
      c->dp = pull8(v, &c->s);
      c->x = pull16(v, &c->s);
      c->y = pull16(v, &c->s);
      c->u = pull16(v, &c->s);
      c->cycles += 9;
    }
    c->pc = pull16(v, &c->s);
    break;
  }
  case 0x3c:
    c->cc &= fetch(v);
    stack_all(v);
    c->waiting = 1;
    break;
  case 0x3d: {
    uint16_t x = (uint16_t)(c->a * c->b);
    sd(c, x);
    c->cc = (c->cc & ~(Z | C)) | (x ? 0 : Z) | (x & 0x80 ? C : 0);
    break;
  }
  case 0x3f:
    stack_all(v);
    c->cc |= I | F;
    c->pc = rw(v, 0xfffa);
    break;
  default:
    c->illegal++;
    break;
  }
}
