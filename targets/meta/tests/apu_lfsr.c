/*
 * gbl_apu_lfsr_steps (gbl_apu_lfsr.h) against the clock-by-clock LFSR of
 * the DMG noise channel, both widths, every step count the APU uses.
 */
#include "gbl_apu_lfsr.h"

#include <stdio.h>

static uint16_t reference(uint16_t lfsr, unsigned steps, int short_mode) {
    while (steps--) {
        uint16_t x = (lfsr ^ (lfsr >> 1)) & 1;
        lfsr = (uint16_t)((lfsr >> 1) | (x << 14));
        if (short_mode) lfsr = (uint16_t)((lfsr & ~0x40) | (x << 6));
    }
    return lfsr;
}

int main(void) {
    int failures = 0;
    for (int mode = 0; mode < 2; mode++)
        for (uint32_t state = 0; state < 0x8000u; state++)
            for (unsigned steps = 0; steps <= 64; steps++) {
                uint16_t want = reference((uint16_t)state, steps, mode);
                uint16_t got = gbl_apu_lfsr_steps((uint16_t)state, steps, mode);
                if (got != want && failures++ < 5)
                    printf("mode %d state %04x steps %u: %04x, want %04x\n", mode, state, steps, got, want);
            }
    printf("apu_lfsr: %s\n", failures ? "MISMATCH" : "every state, 0-64 clocks, both widths exact");
    return failures ? 1 : 0;
}
