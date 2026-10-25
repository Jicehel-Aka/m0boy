/* Idle loops, shared by the recompilers' runtimes: a game waiting for its
 * interrupt in a loop that does nothing but jump to itself (JMP to itself
 * on the 6502, JR $ or JP $ on the Z80).  Interpreted or recompiled
 * instruction by instruction, it spins until the next event (the end of
 * the NES's line slice, the Z80 runtime's next device event); spent at
 * once, it costs a multiply.  The count is the interpreter's: the loops
 * that run before the time left is used up, ceil(left / period), so the
 * cycles land exactly where spinning would have left them (nothing that
 * could cut the wait short changes before that event).
 *
 * The Cortex-M0+ has no divider: the quotient is a multiply by
 * ceil(2^16 / period), exact while left stays under the bound below (a
 * constant when period is), else a 32x32 -> 64 multiply by
 * ceil(2^32 / period). */
#ifndef M0_IDLE_H
#define M0_IDLE_H
#include <stdint.h>

static inline uint32_t m0_idle_loops(uint32_t left, uint32_t period) {
    const uint32_t x = left + period - 1u;
    const uint32_t m = (65536u + period - 1u) / period, excess = m * period - 65536u;
    if (x < 65536u && (!excess || x < 65536u / excess)) /* the rounding error stays under one */
        return (x * m) >> 16;
    return (uint32_t)(((uint64_t)x * ((0xFFFFFFFFu / period) + 1u)) >> 32);
}

#endif
