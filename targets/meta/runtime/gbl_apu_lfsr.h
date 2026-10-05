/*
 * Noise channel LFSR of the gbl APU, advanced `steps` clocks at once.
 *
 * 15-bit mode: each clock shifts right and feeds bit0 ^ bit1 into bit 14,
 * so the k bits fed by k clocks (k <= 14) are s[i] ^ s[i+1] of the current
 * state: one shift and one xor per chunk of 14 instead of a loop per clock
 * (the per-clock loop was two thirds of gbl_apu_render with high-pitched
 * noise, up to 64 clocks per sample).  7-bit mode (NR43 bit 3) also copies
 * the fed bit into bit 6, which the next clocks read: clock by clock, as
 * before.  tests/apu_lfsr.c checks it against the per-clock definition.
 */
#ifndef GBL_APU_LFSR_H
#define GBL_APU_LFSR_H

#include <stdint.h>

static inline uint16_t gbl_apu_lfsr_steps(uint16_t lfsr, uint32_t steps, int short_mode) {
    uint32_t s = lfsr;
    if (short_mode) {
        while (steps--) {
            uint32_t x = (s ^ (s >> 1)) & 1u;
            s = (s >> 1) | (x << 14);
            s = (s & ~0x40u) | (x << 6);
        }
        return (uint16_t)s;
    }
    while (steps) {
        uint32_t k = steps < 14u ? steps : 14u;
        uint32_t x = (s ^ (s >> 1)) & ((1u << k) - 1u);
        s = (s >> k) | (x << (15u - k));
        steps -= k;
    }
    return (uint16_t)s;
}

#endif
