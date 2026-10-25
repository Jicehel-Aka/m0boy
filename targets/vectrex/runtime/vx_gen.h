/* What recompiled 6809 code (vx_gen.c, written by the recompiler's
 * src/system/vectrex/emitter.cpp) needs from the runtime, and what the
 * dispatcher (m6809_run.c under VX_NATIVE) needs from it.
 *
 * A region is one C function over neighbouring instructions of the
 * cartridge or the system ROM.  The registers stay in the machine, as in
 * the run loop (Thumb-1 has eight low registers: locals spill anyway, and
 * the VIA's spin detector reads them); the clock and the slice's end are
 * locals.  The clock is the current instruction's first cycle: an access
 * through the VIA gets it as `start`, plus the cycles the interpreter has
 * counted at that point, and the instruction adds its whole count at its
 * end.  The flags an instruction writes are computed only where a later
 * instruction (or an exit, or a VIA read) can see them. */
#ifndef VX_GEN_H
#define VX_GEN_H

#include "vectrex.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a region: runs from offset `off` of its first address until the slice
 * ends or control leaves it; returns the region that holds the next pc
 * when it is known (a hint, checked), -1 when it is not, -2 when nothing
 * was compiled at that offset */
typedef int (*vx_region_fn)(Vectrex *v, uint32_t stop, uint32_t off);

/* generated */
extern const unsigned vx_gen_regions;
extern const uint16_t vx_gen_first[], vx_gen_last[];
extern const vx_region_fn vx_gen_fn[];
/* 0, or the system ROM's checksum the code was compiled against: its
 * constants (the BIOS's code and data) are folded in */
extern const uint32_t vx_gen_bios_sum;
int vx_gen_find(uint32_t pc);

/* 0: everything through the run loop (the reference, --interp) */
extern int vx_native;
uint32_t vx_bios_sum(const uint8_t *bios);

/* the bus outside plain memory, as the run loop's slow path: `info` is
 * the cycles counted so far | pc << 8 (0: a read that cannot be IFR's,
 * where only the spin detector looks at pc); the clock may move (a
 * skipped spin), the caller reloads it from cpu.start */
uint32_t vxg_read(Vectrex *v, uint32_t addr, uint32_t start, uint32_t info);
/* returns v->attention: the dispatcher must look before the next
 * instruction (an IER write) */
uint32_t vxg_write(Vectrex *v, uint32_t addr_data, uint32_t start, uint32_t counted);

/* PSHS/PSHU, PULS/PULU (and JSR's, RTS's) off the fast path: `op` is the
 * mask, | 0x100 for the U stack; pushes return v->attention, pulls the pc
 * they pulled (or 0) */
uint32_t vxg_push(Vectrex *v, uint32_t op, uint32_t pc, uint32_t start, uint32_t counted);
uint32_t vxg_pull(Vectrex *v, uint32_t op, uint32_t start, uint32_t info);

/* a stack block of n bytes from s within the RAM's first mirror: plain
 * stores and loads (the order of RAM accesses is nobody's business) */
#define VXG_STACK_RAM(s_, n_) ((uint32_t)((s_) - 0xC800u) <= 0x400u - (n_))

/* the fast paths in line, the slow ones moved out of the way */
#ifndef VXG_LIKELY
#define VXG_LIKELY(x) __builtin_expect((x), 1)
#endif

#define VXG_CC_C 1u
#define VXG_CC_V 2u
#define VXG_CC_Z 4u
#define VXG_CC_N 8u
#define VXG_CC_H 32u

#define VXG_PROLOGUE()                                                         \
  M6809 *const c = &v->cpu;                                                    \
  uint8_t *const ram_ = v->ram;                                                \
  const uint8_t *const *const rmap = v->rmap;                                  \
  uint8_t *const *const wmap = v->wmap;                                        \
  uint32_t cycles = c->cycles;                                                 \
  uint32_t pc_;                                                                \
  int hint_;                                                                   \
  const uint8_t *dpr_;                                                         \
  uint8_t *dpw_;                                                               \
  VXG_DP_LOAD();                                                               \
  (void)ram_;                                                                  \
  (void)rmap;                                                                  \
  (void)wmap

/* the direct page through the maps, as pointers to its 256 bytes (0: the
 * VIA's page or the unmapped space); reloaded wherever DP changes */
#define VXG_DP_LOAD()                                                          \
  do {                                                                         \
    const uint32_t i_ = c->dp >> 2, o_ = (c->dp & 3u) << 8;                    \
    dpr_ = rmap[i_] ? rmap[i_] + o_ : 0;                                       \
    dpw_ = wmap[i_] ? wmap[i_] + o_ : 0;                                       \
  } while (0)
#define VXG_DRD(off_, info_)                                                   \
  (__extension__({                                                             \
    uint32_t r_;                                                               \
    if (VXG_LIKELY(dpr_ != 0))                                        \
      r_ = dpr_[(off_)];                                                       \
    else {                                                                     \
      r_ = vxg_read(v, (uint32_t)c->dp << 8 | (off_), cycles, (info_));        \
      cycles = c->start;                                                       \
    }                                                                          \
    r_;                                                                        \
  }))
#define VXG_DWR(off_, val_, counted_)                                          \
  do {                                                                         \
    const uint32_t d_ = (val_) & 0xffu;                                        \
    if (VXG_LIKELY(dpw_ != 0))                                        \
      dpw_[(off_)] = (uint8_t)d_;                                              \
    else if (vxg_write(v, ((uint32_t)c->dp << 8 | (off_)) | d_ << 16, cycles,  \
                       (counted_)))                                            \
      stop = cycles;                                                           \
  } while (0)

/* a byte through the page maps; `info` as vxg_read's */
#define VXG_RD(addr_, info_)                                                   \
  (__extension__({                                                             \
    const uint32_t a_ = (addr_);                                               \
    const uint8_t *const m_ = rmap[a_ >> 10];                                  \
    uint32_t r_;                                                               \
    if (VXG_LIKELY(m_ != 0))                                          \
      r_ = m_[a_ & 1023u];                                                     \
    else {                                                                     \
      r_ = vxg_read(v, a_, cycles, (info_));                                   \
      cycles = c->start;                                                       \
    }                                                                          \
    r_;                                                                        \
  }))
/* the VIA (or anything else off the maps) at a constant address */
#define VXG_IO_RD(addr_, info_)                                                \
  (__extension__({                                                             \
    const uint32_t r_ = vxg_read(v, (addr_), cycles, (info_));                 \
    cycles = c->start;                                                         \
    r_;                                                                        \
  }))
#define VXG_WR(addr_, val_, counted_)                                          \
  do {                                                                         \
    const uint32_t a_ = (addr_), d_ = (val_) & 0xffu;                          \
    uint8_t *const m_ = wmap[a_ >> 10];                                        \
    if (VXG_LIKELY(m_ != 0))                                          \
      m_[a_ & 1023u] = (uint8_t)d_;                                            \
    else if (vxg_write(v, a_ | d_ << 16, cycles, (counted_)))                  \
      stop = cycles;                                                           \
  } while (0)
#define VXG_IO_WR(addr_, val_, counted_)                                       \
  do {                                                                         \
    if (vxg_write(v, (uint32_t)(addr_) | ((uint32_t)(val_) & 0xffu) << 16,     \
                  cycles, (counted_)))                                         \
      stop = cycles;                                                           \
  } while (0)

#define VXG_D() (((uint32_t)c->a << 8) | c->b)
#define VXG_SETD(val_)                                                         \
  do {                                                                         \
    const uint32_t e_ = (val_);                                                \
    c->a = (uint8_t)(e_ >> 8);                                                 \
    c->b = (uint8_t)e_;                                                        \
  } while (0)

/* the flags: `live` (a constant) is the set a later reader can see; the
 * others keep their old value, which nobody reads before it is written */
static inline __attribute__((always_inline)) void vxg_flags(M6809 *c, uint32_t live, uint32_t f) {
  if (live)
    c->cc = (uint8_t)((c->cc & ~live) | (f & live));
}
#define VXG_N8(z) (((z) >> 4) & 8u)
#define VXG_Z8(z) ((z) ? 0u : 4u)
#define VXG_N16(z) (((z) >> 12) & 8u)
static inline __attribute__((always_inline)) uint32_t
vxg_add8(M6809 *c, uint32_t a, uint32_t b, uint32_t carry, uint32_t live) {
  const uint32_t r = a + b + carry, z = r & 0xffu;
  if (live)
    vxg_flags(c, live,
              VXG_N8(z) | VXG_Z8(z) | ((((~(a ^ b)) & (a ^ z)) >> 6) & 2u) |
                  ((r >> 8) & 1u) | ((a ^ b ^ z) & 0x10u) << 1);
  return z;
}
static inline __attribute__((always_inline)) uint32_t
vxg_sub8(M6809 *c, uint32_t a, uint32_t b, uint32_t carry, uint32_t live) {
  const uint32_t r = a - b - carry, z = r & 0xffu;
  if (live)
    vxg_flags(c, live,
              VXG_N8(z) | VXG_Z8(z) | ((((a ^ b) & (a ^ z)) >> 6) & 2u) |
                  ((r >> 8) & 1u));
  return z;
}
static inline __attribute__((always_inline)) uint32_t
vxg_add16(M6809 *c, uint32_t a, uint32_t b, uint32_t live) {
  const uint32_t r = a + b, z = r & 0xffffu;
  if (live)
    vxg_flags(c, live,
              VXG_N16(z) | VXG_Z8(z) | ((((~(a ^ b)) & (a ^ z)) >> 14) & 2u) |
                  ((r >> 16) & 1u));
  return z;
}
static inline __attribute__((always_inline)) uint32_t
vxg_sub16(M6809 *c, uint32_t a, uint32_t b, uint32_t live) {
  const uint32_t r = a - b, z = r & 0xffffu;
  if (live)
    vxg_flags(c, live,
              VXG_N16(z) | VXG_Z8(z) | ((((a ^ b) & (a ^ z)) >> 14) & 2u) |
                  ((r >> 16) & 1u));
  return z;
}
/* N Z set, V cleared: loads, stores, logic */
#define VXG_NZ8(z_, live_) vxg_flags(c, (live_), VXG_N8(z_) | VXG_Z8(z_))
#define VXG_NZ16(z_, live_) vxg_flags(c, (live_), VXG_N16(z_) | VXG_Z8(z_))

/* NEG .. CLR (lo, the opcode's low nibble, a constant); TST returns the
 * operand */
static inline __attribute__((always_inline)) uint32_t
vxg_unary(M6809 *c, unsigned lo, uint32_t o, uint32_t live) {
  uint32_t z = o;
  switch (lo) {
  case 0:
    return vxg_sub8(c, 0, o, 0, live);
  case 3:
    z = ~o & 0xffu;
    vxg_flags(c, live, VXG_N8(z) | VXG_Z8(z) | VXG_CC_C);
    break;
  case 4:
    z = o >> 1;
    vxg_flags(c, live, VXG_Z8(z) | (o & 1u));
    break;
  case 6:
    z = (o >> 1) | ((c->cc & 1u) << 7);
    vxg_flags(c, live, VXG_N8(z) | VXG_Z8(z) | (o & 1u));
    break;
  case 7:
    z = (o >> 1) | (o & 0x80u);
    vxg_flags(c, live, VXG_N8(z) | VXG_Z8(z) | (o & 1u));
    break;
  case 8:
    z = (o << 1) & 0xffu;
    vxg_flags(c, live, VXG_N8(z) | VXG_Z8(z) | (o >> 7) | (((o ^ z) >> 6) & 2u));
    break;
  case 9:
    z = ((o << 1) | (c->cc & 1u)) & 0xffu;
    vxg_flags(c, live, VXG_N8(z) | VXG_Z8(z) | (o >> 7) | (((o ^ z) >> 6) & 2u));
    break;
  case 10:
    z = (o - 1u) & 0xffu;
    vxg_flags(c, live, VXG_N8(z) | VXG_Z8(z) | (o == 0x80u ? 2u : 0u));
    break;
  case 12:
    z = (o + 1u) & 0xffu;
    vxg_flags(c, live, VXG_N8(z) | VXG_Z8(z) | (o == 0x7fu ? 2u : 0u));
    break;
  case 13:
    vxg_flags(c, live, VXG_N8(o) | VXG_Z8(o));
    break;
  default: /* 15 */
    z = 0;
    vxg_flags(c, live, VXG_CC_Z);
    break;
  }
  return z;
}

/* the branch conditions (op's low nibble, a constant) */
static inline __attribute__((always_inline)) int vxg_cond(uint32_t cc, unsigned op) {
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

#ifdef __cplusplus
}
#endif

#endif
