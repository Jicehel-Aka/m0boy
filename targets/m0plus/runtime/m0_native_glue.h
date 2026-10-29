#ifndef M0BOY_M0_NATIVE_GLUE_H
#define M0BOY_M0_NATIVE_GLUE_H
/* Vocabulary of the generated Thumb code (m0_native_page_*.c).  A page
 * function runs chains of instructions: registers and flags live in the CPU
 * struct, cycles accumulate in `c` and reach the clock when the chain ends,
 * and the machine (timers, DMA, NVIC) is looked at only where something can
 * have changed:
 *   - at the start of a block, where `room` (cycles before the next deadline,
 *     samd21_room) must cover the block's longest path; otherwise the
 *     instruction goes to the interpreter, which ticks after every one;
 *   - after an instruction that reached the machine (an access outside SRAM,
 *     a poll-loop skip, PRIMASK), which ends the chain: the access saw the
 *     right time because the cycles so far were put on the clock first.
 * Same cycles and side effects as m0_cpu_execute, instruction by instruction. */
#include "m0_cpu.h"
#include "m0_machine.h"
#include <string.h>

extern const uint8_t m0_rom[];
#ifndef M0_FLASH_BASE
#error "M0_FLASH_BASE missing"
#endif
#define TGT_META 0
#define TGT_POKITTO 1
#define T TGT_META
#define regs (m->r)
#define fN (m->n)
#define fZ (m->z)
#define fC (m->c)
#define fV (m->v)
#define primask (m->primask)
#define irq_work() (hw = 1)
#define exc_active() ((uint32_t)m->exception)
#define pk_blx(op) ((void)0)
#define stepTicks m0_native_sink
#define setNZ(r) m0_native_nz(m, (r))
#define setNZm(r, live) m0_native_nzm(m, (r), (live))
#define addSetCondM(a, b, k, live) m0_native_addm(m, (a), (b), (unsigned)(k), (live))
#define addSetCond(a, b, k) m0_native_add(m, (a), (b), (unsigned)(k))
#define BRANCH(t) do { cyc = 2; br = 1; bt = (t); } while (0)
#define LD32(a) m0_ld(m, (a), 4, &c, &hw)
#define LD16(a) m0_ld(m, (a), 2, &c, &hw)
#define LD8(a) m0_ld(m, (a), 1, &c, &hw)
#define ST32(a, v) m0_st(m, (a), (v), 4, &c, &hw)
#define ST16(a, v) m0_st(m, (a), (v), 2, &c, &hw)
#define ST8(a, v) m0_st(m, (a), (v), 1, &c, &hw)
#define stm_t(t, a, list, n) m0_stm(m, (a), (list), &c, &hw)
#define ldm_t(t, a, list, n, wp) m0_ldm(m, (a), (list), &c, &hw)
/* a short loop that stores nothing: if its registers repeat, the wait is spent */
#define loop_spin(pc) do { c += m0_native_loop(m, (pc), c); } while (0)
#define idle_spin(pc, addr) do { m->cycles += c; c = 0; hw = 1; m0_native_idle(m, (pc), (addr)); } while (0)

static uint32_t m0_native_sink;
/* Flag-setting with only the flags somebody reads (the generator's liveness
 * says which: 8=N 4=Z 2=C 1=V); a constant `live` folds the rest away. */
static inline void m0_native_nzm(M0Cpu *m, uint32_t value, unsigned live) {
    if (live & 8u) m->n = (uint8_t)(value >> 31);
    if (live & 4u) m->z = (uint8_t)(value == 0);
}
static inline uint32_t m0_native_addm(M0Cpu *m, uint32_t a, uint32_t b, unsigned carry, unsigned live) {
    const uint32_t value = a + b + carry;
    if (live & 2u) m->c = (uint8_t)(carry ? value <= a : value < a);
    if (live & 1u) m->v = (uint8_t)((~(a ^ b) & (a ^ value)) >> 31);
    m0_native_nzm(m, value, live);
    return value;
}
static inline uint32_t m0_bswap32(uint32_t x) {
    return x << 24 | (x & 0xff00u) << 8 | (x >> 8 & 0xff00u) | x >> 24;
}

/* Anything but SRAM goes to the board (samd21.c): flash reads and the few
 * registers it can serve without ending the chain run there, the rest puts
 * the chain's cycles on the clock first and sets *hw. */
uint32_t m0_native_ld_slow(M0Cpu *m, uint32_t a, unsigned n, uint32_t *c, int *hw);
void m0_native_st_slow(M0Cpu *m, uint32_t a, uint32_t v, unsigned n, uint32_t *c, int *hw);

static inline uint32_t m0_ld(M0Cpu *m, uint32_t a, unsigned n, uint32_t *c, int *hw) {
    M0Machine *b = (M0Machine *)m->board;
    uint32_t o = a - b->ram_base, v = 0;
    if (__builtin_expect(o <= M0_MACHINE_RAM_BYTES - n, 1)) { memcpy(&v, b->ram + o, n); return v; }
    return m0_native_ld_slow(m, a, n, c, hw);
}
static inline void m0_st(M0Cpu *m, uint32_t a, uint32_t v, unsigned n, uint32_t *c, int *hw) {
    M0Machine *b = (M0Machine *)m->board;
    uint32_t o = a - b->ram_base;
    if (__builtin_expect(o <= M0_MACHINE_RAM_BYTES - n, 1)) {
        /* stores below the stack pointer of the loop being watched are dead
         * once the call returns: they do not make an iteration differ */
        m->stores += a >= m->dead_below;
        memcpy(b->ram + o, &v, n);
        return;
    }
    ++m->stores;
    m0_native_st_slow(m, a, v, n, c, hw);
}
static inline void m0_stm(M0Cpu *m, uint32_t a, uint32_t list, uint32_t *c, int *hw) {
    for (; list; list &= list - 1, a += 4) m0_st(m, a, m->r[__builtin_ctz(list)], 4, c, hw);
}
static inline void m0_ldm(M0Cpu *m, uint32_t a, uint32_t list, uint32_t *c, int *hw) {
    for (; list; list &= list - 1, a += 4) m->r[__builtin_ctz(list)] = m0_ld(m, a, 4, c, hw);
}
static inline void m0_native_branch(M0Cpu *m, uint32_t t) {
    if (t >= 0xfffffff0u) m0_cpu_exc_return(m, t);
    else m->r[15] = t & ~1u;
}
/* a poll loop (load, compare, branch back) whose value only an event can
 * change: the board spends the wait up to its next deadline */
void m0_native_idle(M0Cpu *m, uint32_t pc, uint32_t addr);
uint32_t m0_native_loop(M0Cpu *m, uint32_t pc, uint32_t pending);
#endif
