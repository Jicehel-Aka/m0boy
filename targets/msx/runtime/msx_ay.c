/* AY-3-8910 (MSX PSG) sample generation: the sg1000_audio_* interface of the
 * SN76489 generator (targets/common/runtime/sg_psg.h), so the frontends and
 * the host harness need no change.
 *
 * The chip is clocked at 1.789773 MHz; tones, noise and envelope count at
 * clock / 8, 223 722 ticks a second.  Per output sample the counters advance
 * by the ticks that fit (a Bresenham pass) and the level at the end of the
 * sample is output, as the SN76489 generator does.  Registers are written
 * through msx_ay_write, which first brings the samples up to the write. */
#include "sg1000.h"
#if SG_MSX
#define AY_TICK_HZ 223722u
#define SG_CLOCK 3579545u

/* AY-3-8910 DAC levels (0..1), scaled to +-40 per channel */
static const uint8_t level[16] = { 0, 1, 1, 1, 2, 2, 3, 5, 7, 11, 14, 18, 23, 27, 34, 40 };

void sg1000_audio_rate(unsigned rate) {
    sg1000_t *m = &machine;
    m->audio_rate = !rate ? 0 : rate > 48000 ? 48000 : rate < 1000 ? 1000 : rate;
    m->audio_div = m->audio_rate;
    m->audio_tick_int = m->audio_div ? AY_TICK_HZ / m->audio_div : 0;
    m->audio_tick_frac = m->audio_div ? AY_TICK_HZ % m->audio_div : 0;
    m->audio_phase = m->audio_tick_phase = 0;
    m->audio_cyc = m->cyc;
    m->audio_head = m->audio_tail = 0;
}

/* A counter that reloads from period every period ticks: advance it by
 * ticks, returning how many times it reloaded. */
static inline unsigned advance(uint16_t *count, unsigned period, unsigned ticks) {
    unsigned reloads = 0, c = *count;
    while (ticks >= c) {
        ticks -= c;
        c = period;
        reloads++;
    }
    *count = (uint16_t)(c - ticks);
    return reloads;
}

static inline unsigned envelope_level(const sg1000_t *m) {
    if (m->ay_env_hold) return m->ay_env_final;
    return m->ay_env_attack ? m->ay_env_pos : 15u - m->ay_env_pos;
}

/* One envelope step: a segment is 16 steps; at its end the shape (C A ALT
 * HOLD in R13) stops the envelope at 0 or 15, or starts another segment. */
static void envelope_step(sg1000_t *m) {
    if (m->ay_env_hold || ++m->ay_env_pos < 16) return;
    const unsigned shape = m->ay_reg[13] & 15;
    m->ay_env_pos = 0;
    if (!(shape & 8)) { m->ay_env_hold = 1; m->ay_env_final = 0; }
    else if (shape & 1) { m->ay_env_hold = 1; m->ay_env_final = (m->ay_env_attack ^ ((shape >> 1) & 1)) ? 15 : 0; }
    else if (shape & 2) m->ay_env_attack ^= 1;
}

static void produce(sg1000_t *m, unsigned n) {
    const uint32_t tint = m->audio_tick_int, tfrac = m->audio_tick_frac, div = m->audio_div;
    while (n--) {
        uint32_t ticks = tint;
        m->audio_tick_phase += tfrac;
        if (m->audio_tick_phase >= div) { m->audio_tick_phase -= div; ticks++; }
        const uint8_t *r = m->ay_reg;
        for (unsigned ch = 0; ch < 3; ch++) {
            unsigned period = (r[ch * 2] | (r[ch * 2 + 1] & 15u) << 8);
            if (period < 1) period = 1;
            uint16_t count = m->ay_count[ch];
            m->ay_out[ch] ^= (uint8_t)(advance(&count, period, ticks) & 1);
            m->ay_count[ch] = count;
        }
        unsigned np = r[6] & 31;
        if (np < 1) np = 1;
        for (unsigned k = advance(&m->ay_noise_count, np * 2, ticks); k; k--)
            m->ay_lfsr = (m->ay_lfsr >> 1) | ((((m->ay_lfsr ^ (m->ay_lfsr >> 3)) & 1) << 16));
        unsigned ep = r[11] | (unsigned)r[12] << 8;
        if (ep < 1) ep = 1;
        for (unsigned k = advance(&m->ay_env_count, ep * 2, ticks); k; k--) envelope_step(m);
        const unsigned noise = m->ay_lfsr & 1, mixer = r[7];
        int sum = 128;
        for (unsigned ch = 0; ch < 3; ch++) {
            const unsigned on = (m->ay_out[ch] | (mixer >> ch)) & (noise | (mixer >> (ch + 3))) & 1;
            const unsigned volume = r[8 + ch];
            const int amp = level[volume & 16 ? envelope_level(m) : volume & 15];
            sum += on ? amp : -amp;
        }
        const unsigned next = (m->audio_head + 1u) & 511u;
        if (next != m->audio_tail) { m->audio_buffer[m->audio_head] = (uint8_t)sum; m->audio_head = (uint16_t)next; }
    }
}

void sg1000_audio_catchup(void) {
    sg1000_t *s = &machine;
    if (!s->audio_rate) return;
    uint32_t elapsed = s->cyc - s->audio_cyc;
    s->audio_cyc = s->cyc;
    unsigned n = 0;
    while (elapsed) { /* chunks keep phase + elapsed * rate below 2^32 */
        const uint32_t chunk = elapsed > 65536 ? 65536 : elapsed;
        elapsed -= chunk;
        const uint32_t phase = s->audio_phase + chunk * s->audio_rate;
        n += phase / SG_CLOCK;
        s->audio_phase = phase % SG_CLOCK;
    }
    if (n) produce(s, n);
}

void sg1000_audio_render(uint8_t *buffer, unsigned n) {
    sg1000_t *s = &machine;
    while (n--) {
        if (s->audio_tail == s->audio_head) *buffer++ = 128;
        else { *buffer++ = s->audio_buffer[s->audio_tail]; s->audio_tail = (uint16_t)((s->audio_tail + 1) & 511); }
    }
}

void msx_ay_write(unsigned reg, uint8_t value) {
    static const uint8_t mask[16] = { 255, 15, 255, 15, 255, 15, 31, 255, 31, 31, 31, 255, 255, 15, 255, 255 };
    sg1000_t *m = &machine;
    reg &= 15;
    sg1000_audio_catchup();
    m->ay_reg[reg] = value & mask[reg];
    if (reg == 13) { /* restarts the envelope */
        m->ay_env_attack = (value >> 2) & 1;
        m->ay_env_pos = 0;
        m->ay_env_hold = 0;
        m->ay_env_count = 0;
    }
}
#endif
