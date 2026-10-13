#include "sg1000.h"
#define SG_CLOCK 3579545u
/* SN76489AN: three square waves, 2 dB attenuation, TI noise LFSR (15 stages
 * plus output delay).  Samples are produced up to machine.cyc before every
 * PSG write and at frame end, so short notes and volume changes keep their
 * position inside a frame. */
static const uint8_t level[16] = { 30, 24, 19, 15, 12, 10, 8, 6, 5, 4, 3, 2, 2, 1, 1, 0 };

void sg1000_audio_rate(unsigned rate) {
    sg1000_t *m = &machine;
    m->audio_rate = rate > 48000 ? 48000 : rate;
    /* PSG clock divider 16: whole ticks per sample plus a remainder */
    m->audio_div = m->audio_rate * 16;
    m->audio_tick_int = m->audio_div ? SG_CLOCK / m->audio_div : 0;
    m->audio_tick_frac = m->audio_div ? SG_CLOCK % m->audio_div : 0;
    m->audio_phase = m->audio_tick_phase = 0;
    m->audio_cyc = m->cyc;
    m->audio_head = m->audio_tail = 0;
}

/* Samples are produced in blocks, one channel at a time over the block (few
 * live values: the M0+ keeps them in registers), then mixed.  Every channel
 * replays the same tick sequence (Bresenham on the PSG clock divided by 16),
 * so the result equals producing the four channels sample by sample. */
#define SG_AUDIO_BLOCK 128

static void tone_block(sg1000_t *s, unsigned ch, int16_t *mix, unsigned n, uint32_t phase) {
    /* A zero tone divider acts as 0x400 on the TI part. */
    const int32_t period = s->psg_tone[ch] ? s->psg_tone[ch] : 1024;
    const int amp = level[s->psg_volume[ch]];
    const uint32_t tint = s->audio_tick_int, tfrac = s->audio_tick_frac, div = s->audio_div;
    int32_t count = s->audio_count[ch];
    unsigned out = s->audio_output[ch];
    for (unsigned i = 0; i < n; i++) {
        uint32_t ticks = tint;
        phase += tfrac;
        if (phase >= div) { phase -= div; ticks++; }
        count -= (int32_t)ticks;
        while (count <= 0) { count += period; out ^= 1; }
        mix[i] = (int16_t)(mix[i] + (out ? amp : -amp));
    }
    s->audio_count[ch] = count;
    s->audio_output[ch] = (uint8_t)out;
}

static void noise_block(sg1000_t *s, int16_t *mix, unsigned n, uint32_t phase) {
    int32_t period = (s->psg_noise & 3) == 3 ? s->psg_tone[2] * 2 : 32 << (s->psg_noise & 3);
    if (!period) period = 2048;
    const int amp = level[s->psg_volume[3]];
    const unsigned white = s->psg_noise & 4;
    const uint32_t tint = s->audio_tick_int, tfrac = s->audio_tick_frac, div = s->audio_div;
    int32_t count = s->audio_count[3];
    uint32_t lfsr = s->audio_lfsr;
    for (unsigned i = 0; i < n; i++) {
        uint32_t ticks = tint;
        phase += tfrac;
        if (phase >= div) { phase -= div; ticks++; }
        count -= (int32_t)ticks;
        while (count <= 0) {
            count += period;
            unsigned feedback = white ? ((lfsr >> 2) ^ (lfsr >> 3)) & 1 : (lfsr >> 2) & 1;
            lfsr = (lfsr >> 1) | (feedback << 16);
        }
        mix[i] = (int16_t)(mix[i] + ((lfsr & 1) ? amp : -amp));
    }
    s->audio_count[3] = count;
    s->audio_lfsr = lfsr;
    s->audio_output[3] = (uint8_t)(lfsr & 1);
}

static void produce(sg1000_t *s, unsigned n) {
    int16_t mix[SG_AUDIO_BLOCK];
    while (n) {
        unsigned k = n > SG_AUDIO_BLOCK ? SG_AUDIO_BLOCK : n;
        n -= k;
        for (unsigned i = 0; i < k; i++) mix[i] = 128;
        const uint32_t phase = s->audio_tick_phase;
        for (unsigned ch = 0; ch < 3; ch++) tone_block(s, ch, mix, k, phase);
        noise_block(s, mix, k, phase);
        /* the shared tick phase after k samples */
        uint32_t p = phase;
        for (unsigned i = 0; i < k; i++) { p += s->audio_tick_frac; if (p >= s->audio_div) p -= s->audio_div; }
        s->audio_tick_phase = p;
        unsigned head = s->audio_head, tail = s->audio_tail;
        for (unsigned i = 0; i < k; i++) {
            unsigned next = (head + 1) & 511;
            if (next == tail) break;
            s->audio_buffer[head] = (uint8_t)mix[i];
            head = next;
        }
        s->audio_head = (uint16_t)head;
    }
}

void sg1000_audio_catchup(void) {
    sg1000_t *s = &machine;
    if (!s->audio_rate) return;
    uint32_t elapsed = s->cyc - s->audio_cyc;
    s->audio_cyc = s->cyc;
    unsigned n = 0;
    while (elapsed) { /* chunks keep elapsed * rate below 2^32 */
        uint32_t chunk = elapsed > 65536 ? 65536 : elapsed;
        elapsed -= chunk;
        s->audio_phase += chunk * s->audio_rate;
        while (s->audio_phase >= SG_CLOCK) { s->audio_phase -= SG_CLOCK; n++; }
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
