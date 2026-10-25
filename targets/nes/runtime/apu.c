#include "apu.h"

/* The sampled APU: every output sample advances each channel by the CPU
 * cycles it spans (fractional cycles carried), and the envelope, length
 * and sweep counters at their own 240/120 Hz marks.  Mixing is the plain
 * sum the console's DACs approximate.  The frame-counter IRQ stays masked:
 * no game in scope waits on it. */
static const uint8_t duty_table[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 0, 0},
};
static const uint8_t triangle_steps[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
};
static const uint16_t noise_period[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};
static const uint8_t length_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

#define NES_APU_CPU_HZ 1789773u
/* 128 + mix * 120 / 42: the sum's 43 values, no division per sample */
static const uint8_t mix_level[43] = {
#define L(m) (uint8_t)(128 + (m) * 120 / 42)
    L(0), L(1), L(2), L(3), L(4), L(5), L(6), L(7), L(8), L(9), L(10), L(11), L(12), L(13), L(14),
    L(15), L(16), L(17), L(18), L(19), L(20), L(21), L(22), L(23), L(24), L(25), L(26), L(27), L(28),
    L(29), L(30), L(31), L(32), L(33), L(34), L(35), L(36), L(37), L(38), L(39), L(40), L(41), L(42),
#undef L
};

void nes_apu_reset(nes_apu_t *apu) {
    for (unsigned i = 0; i < 4; i++) {
        apu->timer[i] = apu->counter[i] = 0;
        apu->length[i] = apu->seq[i] = 0;
    }
    apu->envelope[0] = apu->envelope[1] = 0;
    apu->duty[0] = apu->duty[1] = 0;
    apu->enable = 0;
    apu->lfsr = 1;
    apu->env_div = apu->len_div = apu->sweep_div[0] = apu->sweep_div[1] = 0;
    apu->triangle_reload = 0;
    apu->sample_frac = 0;
}

void nes_apu_write(nes_apu_t *apu, unsigned reg, uint8_t value) {
    const unsigned voice = reg >> 2;
    if (voice > 3) {
        if (reg == 0x15) {
            apu->enable = value & 0x1F;
            for (unsigned i = 0; i < 4; i++)
                if (!(value & (1u << i))) apu->length[i] = 0;
        }
        return; /* $4017: the IRQ stays masked whatever the game asks */
    }
    switch (reg & 3) {
    case 0:
        if (voice < 2) {
            apu->duty[voice] = value >> 6;
            apu->envelope_halt[voice] = value & 0x20;
            apu->envelope_const[voice] = value & 0x10;
            apu->envelope_vol[voice] = value & 0x0F;
        } else if (voice == 2) apu->triangle_halt = value & 0x80;
        else {
            apu->envelope3_halt = value & 0x20;
            apu->envelope3_const = value & 0x10;
            apu->envelope3_vol = value & 0x0F;
        }
        break;
    case 1:
        if (voice < 2) {
            apu->sweep_on[voice] = value & 0x80;
            apu->sweep_period[voice] = (value >> 4) & 7;
            apu->sweep_negate[voice] = value & 0x08;
            apu->sweep_shift[voice] = value & 7;
            apu->sweep_reload[voice] = 1;
        }
        break;
    case 2:
        apu->timer[voice] = (uint16_t)((apu->timer[voice] & 0x0700) | value);
        if (voice == 3) apu->timer[3] = noise_period[value & 0x0F];
        break;
    case 3:
        apu->timer[voice] = (uint16_t)((apu->timer[voice] & 0x00FF) | ((value & 7) << 8));
        if (voice == 3) apu->timer[3] = noise_period[value & 0x0F];
        if (apu->enable & (1u << voice)) {
            apu->length[voice] = length_table[(value >> 3) & 0x1F];
            if (voice < 2) apu->seq[voice] = 0;
        }
        if (voice == 2) apu->triangle_reload = 1;
        break;
    }
}

uint8_t nes_apu_read(nes_apu_t *apu) {
    uint8_t out = 0;
    for (unsigned i = 0; i < 4; i++)
        if (apu->length[i]) out |= 1u << i;
    return out;
}

static void envelope_clock(nes_apu_t *apu) {
    for (unsigned i = 0; i < 2; i++) {
        if (apu->envelope_const[i]) continue;
        if (apu->envelope[i]) apu->envelope[i]--;
        else if (apu->envelope_halt[i]) apu->envelope[i] = 15;
    }
    if (!apu->envelope3_const) {
        if (apu->envelope3) apu->envelope3--;
        else if (apu->envelope3_halt) apu->envelope3 = 15;
    }
}
static void sweep_clock(nes_apu_t *apu, unsigned i) {
    if (apu->sweep_reload[i]) { apu->sweep_reload[i] = 0; apu->sweep_div[i] = apu->sweep_period[i]; return; }
    if (apu->sweep_div[i]) apu->sweep_div[i]--;
    else {
        apu->sweep_div[i] = apu->sweep_period[i];
        if (apu->sweep_on[i] && apu->sweep_shift[i] && apu->timer[i] >= 8) {
            const unsigned delta = apu->timer[i] >> apu->sweep_shift[i];
            const unsigned target = apu->sweep_negate[i]
                                        ? apu->timer[i] - delta - (i ? 1u : 0u)
                                        : apu->timer[i] + delta;
            if (target < 0x800) apu->timer[i] = (uint16_t)target;
        }
    }
}

/* A channel's timer over `run` cycles, in closed form: the per-cycle rule
 * is "at 0, reload the period and step, else count down", so the first
 * step comes after counter + 1 cycles and the next ones every timer + 1.
 * Returns the steps taken; the counter is left where the cycles leave it. */
static inline unsigned timer_run(uint16_t *counter, unsigned timer, unsigned run) {
    unsigned c = *counter;
    if (run <= c) { *counter = (uint16_t)(c - run); return 0; }
    run -= c + 1;
    unsigned steps = 1;
    if (timer < 32) { /* short periods (or none: a silent voice's 0) divide */
        steps += run / (timer + 1);
        run %= timer + 1;
    } else {
        while (run > timer) { run -= timer + 1; steps++; }
    }
    *counter = (uint16_t)(timer - run);
    return steps;
}

void nes_apu_render(nes_apu_t *apu, uint8_t *dst, unsigned n, unsigned rate) {
    /* whole CPU cycles per sample, the remainder carried: the quotient and
     * the remainder once, an add and a compare per sample (sample_frac
     * stays below rate) */
    const unsigned whole = NES_APU_CPU_HZ / rate, part = NES_APU_CPU_HZ % rate;
    for (unsigned s = 0; s < n; s++) {
        apu->sample_frac += part;
        unsigned left = whole;
        if (apu->sample_frac >= rate) { apu->sample_frac -= rate; left++; }
        /* the channels run in closed form up to the next frame-sequencer
         * mark, which then applies after that cycle's steps: the same
         * order as clocking them cycle by cycle (1.8 million iterations a
         * second, more than the SAMD21 has) */
        while (left) {
            unsigned run = left;
            const unsigned to_env = NES_APU_CPU_HZ / 240 - apu->env_div;
            const unsigned to_len = NES_APU_CPU_HZ / 120 - apu->len_div;
            if (run > to_env) run = to_env;
            if (run > to_len) run = to_len;
            left -= run;
            /* the pulses and the triangle step when their period elapses */
            for (unsigned i = 0; i < 2; i++)
                apu->seq[i] = (uint8_t)((apu->seq[i] + timer_run(&apu->counter[i], apu->timer[i], run)) & 7);
            /* the triangle's step: its condition holds for the whole run
             * (only the else branch changes it, and only to keep it false) */
            const unsigned k = timer_run(&apu->counter[2], apu->timer[2], run);
            if (k) {
                if (apu->length[2] && (apu->triangle_linear || apu->triangle_reload))
                    apu->seq[2] = (uint8_t)((apu->seq[2] + k) & 31);
                else if (!apu->triangle_halt) apu->triangle_reload = 0;
            }
            for (unsigned k = timer_run(&apu->counter[3], apu->timer[3], run); k; k--) {
                const uint16_t bit = (uint16_t)((apu->lfsr ^ (apu->lfsr >> 1)) & 1);
                apu->lfsr = (uint16_t)((apu->lfsr >> 1) | (bit << 14));
            }
            /* the frame rates: 240 Hz envelopes, 120 Hz lengths and sweeps */
            apu->env_div += run;
            if (apu->env_div >= NES_APU_CPU_HZ / 240) { apu->env_div = 0; envelope_clock(apu); }
            apu->len_div += run;
            if (apu->len_div >= NES_APU_CPU_HZ / 120) {
                apu->len_div = 0;
                for (unsigned i = 0; i < 4; i++) {
                    const int halted = i < 2 ? apu->envelope_halt[i] : i == 2 ? apu->triangle_halt : 0;
                    if (apu->length[i] && !halted) apu->length[i]--;
                }
                sweep_clock(apu, 0);
                sweep_clock(apu, 1);
            }
        }
        unsigned mix = 0;
        for (unsigned i = 0; i < 2; i++) {
            if (!apu->length[i] || apu->timer[i] < 8) continue;
            const unsigned volume = apu->envelope_const[i] ? apu->envelope_vol[i] : apu->envelope[i];
            mix += duty_table[apu->duty[i]][apu->seq[i]] * volume;
        }
        if (apu->length[2]) mix += triangle_steps[apu->seq[2]] >> 1;
        if (apu->length[3] && !(apu->lfsr & 1)) mix += apu->envelope3_const ? apu->envelope3_vol : apu->envelope3;
        /* two pulses up to 15, the triangle up to 7, the noise up to 15 */
        dst[s] = mix_level[mix > 42 ? 42 : mix];
    }
}
