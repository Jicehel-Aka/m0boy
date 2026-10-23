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
#define SG_AY_BLOCK 64
#define SG_CLOCK 3579545u

/* AY-3-8910 DAC levels (0..1), scaled to +-40 per channel */
static const uint8_t level[16] = { 0, 1, 1, 1, 2, 2, 3, 5, 7, 11, 14, 18, 23, 27, 34, 40 };

void sg1000_audio_rate(unsigned rate) {
    sg1000_t *m = &machine;
    m->audio_rate = !rate ? 0 : rate > 48000 ? 48000 : rate < 1000 ? 1000 : rate;
    m->audio_div = m->audio_rate;
    m->audio_tick_int = m->audio_div ? AY_TICK_HZ / m->audio_div : 0;
    m->audio_tick_frac = m->audio_div ? AY_TICK_HZ % m->audio_div : 0;
    m->scc_k = m->audio_rate ? (uint32_t)(((uint64_t)3579545 << 16) / m->audio_rate) : 0; /* SCC wave steps per output sample, 16.16 */
    for (unsigned c = 0; c < 5; c++) m->scc_inc[c] = (m->scc_k / ((unsigned)m->scc_period[c] + 1)) << 11;
    m->audio_phase = m->audio_tick_phase = 0;
    m->audio_cyc = m->cyc;
    m->audio_head = m->audio_tail = 0;
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

/* Registers do not change inside a call (msx_ay_write catches up first), so
 * what each channel needs is worked out once.  A block of samples is produced
 * a source at a time, as the SN76489 generator does: the ticks of every
 * sample once, then the noise bit and the envelope level of every sample when
 * an audible channel uses them, then each channel over the block.  A silent
 * channel, or one whose tone and noise are both off, is a constant and costs
 * no counting. */

/* The loops of produce, apart so that each keeps its few values in registers. */
/* a square channel on a fixed volume: +-a at the tone's phase */
static __attribute__((noinline)) void ay_tone(int16_t *mix, const uint8_t *ticks, unsigned k, int32_t *count_io, unsigned *out_io, int32_t p, int a) {
    int32_t count = *count_io;
    unsigned out = *out_io;
    const int16_t level2[2] = { (int16_t)-a, (int16_t)a };
    for (unsigned i = 0; i < k; i++) {
        count -= ticks[i];
        if (count <= 0) { do { count += p; out ^= 1; } while (count <= 0); }
        mix[i] = (int16_t)(mix[i] + level2[out]);
    }
    *count_io = count;
    *out_io = out;
}
/* one SCC voice: the phase has its 5-bit step in the top bits (32-step wave), so the index is a shift */
static __attribute__((noinline)) uint32_t scc_voice(int16_t *mix, unsigned k, uint32_t phase, uint32_t inc, const int8_t *w, int vol) {
    for (unsigned i = 0; i < k; i++) {
        phase += inc;
        mix[i] = (int16_t)(mix[i] + ((w[phase >> 27] * vol) >> 6));
    }
    return phase;
}

static void produce(sg1000_t *m, unsigned n) {
    const uint32_t tint = m->audio_tick_int, tfrac = m->audio_tick_frac, div = m->audio_div;
    const uint8_t *r = m->ay_reg;
    const unsigned mixer = r[7];
    unsigned period[3], enveloped = 0, noisy = 0, counting = 0;
    int fixed = m->click_used ? (m->click ? 12 : -12) : 0; /* the click, and below the channels that never toggle, summed */
    uint8_t volume[3], mode[3];             /* mode: 1 tone, 2 noise */
    for (unsigned ch = 0; ch < 3; ch++) {
        volume[ch] = r[8 + ch];
        const unsigned tone = !((mixer >> ch) & 1), noise = !((mixer >> (ch + 3)) & 1);
        mode[ch] = 0;
        period[ch] = 0;
        if (!(volume[ch] & 16) && !(volume[ch] & 15)) continue; /* silent */
        if (volume[ch] & 16) enveloped = 1;
        if (noise) { noisy = 1; mode[ch] |= 2; }
        if (tone) {
            period[ch] = r[ch * 2] | (r[ch * 2 + 1] & 15u) << 8;
            if (period[ch] < 1) period[ch] = 1;
            counting |= 1u << ch;
            mode[ch] |= 1;
        }
        if (!mode[ch] && !(volume[ch] & 16)) fixed += level[volume[ch] & 15]; /* a constant high */
    }
    unsigned np = r[6] & 31, ep = r[11] | (unsigned)r[12] << 8;
    if (np < 1) np = 1;
    if (ep < 1) ep = 1;
    int16_t mix[SG_AY_BLOCK];
    uint8_t ticks[SG_AY_BLOCK], noise_bit[SG_AY_BLOCK], env_level[SG_AY_BLOCK];
    while (n) {
        const unsigned k = n > SG_AY_BLOCK ? SG_AY_BLOCK : n;
        n -= k;
        uint32_t phase = m->audio_tick_phase;
        for (unsigned i = 0; i < k; i++) {
            uint32_t t = tint;
            phase += tfrac;
            if (phase >= div) { phase -= div; t++; }
            ticks[i] = (uint8_t)t;
            mix[i] = (int16_t)(128 + fixed);
        }
        m->audio_tick_phase = phase;
        if (noisy) {
            uint32_t lfsr = m->audio_lfsr;
            int32_t count = m->ay_noise_count;
            const int32_t period_n = (int32_t)np * 2;
            for (unsigned i = 0; i < k; i++) {
                count -= ticks[i];
                while (count <= 0) { count += period_n; lfsr = (lfsr >> 1) | ((((lfsr ^ (lfsr >> 3)) & 1) << 16)); }
                noise_bit[i] = (uint8_t)(lfsr & 1);
            }
            m->audio_lfsr = lfsr;
            m->ay_noise_count = (uint16_t)count;
        }
        if (enveloped) {
            int32_t count = m->ay_env_count;
            const int32_t period_e = (int32_t)ep * 2;
            for (unsigned i = 0; i < k; i++) {
                count -= ticks[i];
                while (count <= 0) { count += period_e; envelope_step(m); }
                env_level[i] = (uint8_t)envelope_level(m);
            }
            m->ay_env_count = (uint16_t)count;
        }
        for (unsigned ch = 0; ch < 3; ch++) {
            const unsigned vol = volume[ch], md = mode[ch];
            if (!md && !(vol & 16)) continue; /* silent, or in fixed */
            const int amp = level[vol & 15];
            const int32_t p = (int32_t)period[ch];
            int32_t count = m->ay_count[ch];
            unsigned out = m->ay_out[ch];
            if (md == 1 && !(vol & 16)) ay_tone(mix, ticks, k, &count, &out, p, amp);
            else
            for (unsigned i = 0; i < k; i++) {
                if (md & 1) { /* the tone counter runs even where noise decides the sample */
                    count -= ticks[i];
                    if (count <= 0) { do { count += p; out ^= 1; } while (count <= 0); }
                }
                const unsigned on = ((md & 1) ? out : 1) & ((md & 2) ? noise_bit[i] : 1);
                const int a = (vol & 16) ? level[env_level[i]] : amp;
                mix[i] = (int16_t)(mix[i] + (on ? a : -a));
            }
            m->ay_count[ch] = (uint16_t)count;
            m->ay_out[ch] = (uint8_t)out;
        }
        for (unsigned c = 0; c < 5; c++) { /* the SCC: 32-step waveforms, each voice a phase accumulator */
            const unsigned vol = m->scc_vol[c];
            if (!(m->scc_enable >> c & 1) || !vol) continue;
            m->scc_phase[c] = scc_voice(mix, k, m->scc_phase[c], m->scc_inc[c], m->scc_wave[c < 4 ? c : 3], (int)vol);
        }
        unsigned head = m->audio_head;
        const unsigned tail = m->audio_tail;
        for (unsigned i = 0; i < k; i++) {
            const unsigned next = (head + 1u) & 511u;
            if (next == tail) break;
            const int v = mix[i];
            m->audio_buffer[head] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
            head = next;
        }
        m->audio_head = (uint16_t)head;
    }
}

void sg1000_audio_catchup(void) {
    sg1000_t *s = &machine;
    if (!s->audio_rate) return;
    uint32_t elapsed = s->cyc - s->audio_cyc;
    s->audio_cyc = s->cyc;
    unsigned n = 0;
    if (elapsed < 65536) { /* no sample due yet, or a few: no division */
        uint32_t due = s->audio_phase + elapsed * s->audio_rate;
        while (due >= SG_CLOCK && n < 4) { due -= SG_CLOCK; n++; }
        if (due < SG_CLOCK) { s->audio_phase = due; elapsed = 0; }
        else n = 0; /* the division below */
    }
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

/* K051649 registers (offset in 9800-98FF): waveforms, periods, volumes, enable */
void msx_scc_write(unsigned reg, uint8_t value) {
    sg1000_t *m = &machine;
    sg1000_audio_catchup();
    if (reg < 0x80) { m->scc_wave[reg >> 5][reg & 31] = (int8_t)value; return; }
    if (reg < 0x8a) {
        const unsigned c = (reg - 0x80) >> 1;
        m->scc_period[c] = (uint16_t)((reg & 1) ? (m->scc_period[c] & 0xff) | (value & 15u) << 8 : (m->scc_period[c] & 0xf00) | value);
        m->scc_inc[c] = (m->scc_k / ((unsigned)m->scc_period[c] + 1)) << 11;
        return;
    }
    if (reg < 0x8f) { m->scc_vol[reg - 0x8a] = value & 15; return; }
    if (reg == 0x8f) m->scc_enable = value & 31;
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
