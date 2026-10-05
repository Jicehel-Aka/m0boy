/*
 * gbl APU: DMG sound synthesized in batches.
 *
 * Register writes update channel state immediately (triggers, envelopes,
 * frequencies).  The frontend asks for samples once per emulated frame with
 * gbl_apu_render(); the frame sequencer (length 256 Hz, sweep 128 Hz,
 * envelope 64 Hz) runs in sample time during rendering.  Register changes
 * are therefore quantized to the render call, which suits music drivers that
 * update once per frame and keeps the per-sample cost low enough for a
 * Cortex-M0+.
 */
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_internal.h"
#include "gbl_apu_lfsr.h"

#include <string.h>

#define NR(n) gbl_io[(n)]

typedef struct {
    uint8_t on;          /* channel enabled (NR52 status) */
    uint8_t dac;
    uint8_t vol;         /* current envelope volume 0..15 */
    uint8_t env_timer;
    uint16_t length;     /* remaining length ticks */
    uint16_t freq;       /* 11-bit period value */
    uint32_t phase;
    uint32_t inc;        /* phase increment per output sample */
} chan_t;

static chan_t ch[4];
static uint8_t sweep_timer, sweep_on;
static uint16_t sweep_shadow;
static uint16_t lfsr;
static uint32_t seq_acc, seq_step_samples;
static uint8_t seq_step;
static uint32_t rate = 22050;
uint8_t gbl_apu_mute; /* debug: bit c silences channel c in the mix */

static const uint8_t duty_table[4] = {0x01, 0x81, 0x87, 0x7E};
static const uint8_t read_mask[0x20] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF,
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF,
    0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
};

/* Phase increments: a full 2^32 phase is one waveform period.  Each is
 * floor(K / (d * rate)) = floor(floor(K / rate) / d): the numerators over
 * rate are set by gbl_apu_set_rate, so a register write costs one or two
 * 32-bit divisions instead of a 64-bit one (about 1,000 cycles on the M0+).
 * square: 8 duty steps at 1048576 / (2048 - f) Hz, K = 2^49;
 * wave: 32 samples at 2097152 / (2048 - f) Hz, K = 2^48;
 * noise: LFSR clocks per sample in 16.16, 2^36 / (2 * rate * 2^s * div). */
static uint32_t square_hi, wave_hi, noise_num;
static uint16_t square_lo, wave_lo;

/* (hi * 65536 + lo) / d for d < 65536, the quotient fitting in 32 bits. */
static uint32_t div48(uint32_t hi, uint16_t lo, uint32_t d) {
    uint32_t q = hi / d, r = hi - q * d;
    return (q << 16) + (((r << 16) | lo) / d);
}
static uint32_t square_inc(uint16_t f) { return div48(square_hi, square_lo, 2048u - f); }
static uint32_t wave_inc(uint16_t f) { return div48(wave_hi, wave_lo, 2048u - f); }
static uint32_t noise_inc(uint8_t nr43) {
    uint32_t r = nr43 & 7, s = nr43 >> 4;
    return (noise_num >> s) / (r ? 2u * r : 1u); /* r = 0 counts as 0.5 */
}

static void update_freq(int c) {
    if (c == 0 || c == 1) ch[c].inc = square_inc(ch[c].freq);
    else if (c == 2) ch[c].inc = wave_inc(ch[c].freq);
}

static uint16_t sweep_calc(void) {
    uint16_t delta = (uint16_t)(sweep_shadow >> (NR(0x10) & 7));
    uint16_t f = (NR(0x10) & 0x08) ? (uint16_t)(sweep_shadow - delta) : (uint16_t)(sweep_shadow + delta);
    if (f > 2047) ch[0].on = 0;
    return f;
}

static void trigger(int c) {
    static const uint8_t nrx2[4] = {0x12, 0x17, 0x1C, 0x21};
    chan_t *h = &ch[c];
    h->on = h->dac;
    if (!h->length) h->length = c == 2 ? 256 : 64;
    if (c != 2) {
        uint8_t e = NR(nrx2[c]);
        h->vol = e >> 4;
        h->env_timer = e & 7;
    }
    if (c == 2) h->phase = 0;
    if (c == 3) lfsr = 0x7FFF;
    if (c == 0) {
        uint8_t nr10 = NR(0x10);
        sweep_shadow = h->freq;
        sweep_timer = (nr10 >> 4) & 7;
        if (!sweep_timer) sweep_timer = 8;
        sweep_on = (nr10 & 0x70) || (nr10 & 7);
        if (nr10 & 7) sweep_calc();
    }
}

static void clock_length(void) {
    static const uint8_t nrx4[4] = {0x14, 0x19, 0x1E, 0x23};
    for (int c = 0; c < 4; c++)
        if ((NR(nrx4[c]) & 0x40) && ch[c].length && --ch[c].length == 0) ch[c].on = 0;
}

static void clock_envelope(void) {
    static const uint8_t nrx2[4] = {0x12, 0x17, 0x00, 0x21};
    for (int c = 0; c < 4; c++) {
        if (c == 2) continue;
        uint8_t e = NR(nrx2[c]);
        if (!(e & 7) || !ch[c].env_timer) continue;
        if (--ch[c].env_timer) continue;
        ch[c].env_timer = e & 7;
        if ((e & 0x08) && ch[c].vol < 15) ch[c].vol++;
        else if (!(e & 0x08) && ch[c].vol > 0) ch[c].vol--;
    }
}

static void clock_sweep(void) {
    if (!sweep_on || !sweep_timer || --sweep_timer) return;
    uint8_t nr10 = NR(0x10);
    sweep_timer = (nr10 >> 4) & 7;
    if (!sweep_timer) { sweep_timer = 8; return; }
    uint16_t f = sweep_calc();
    if (f <= 2047 && (nr10 & 7)) {
        sweep_shadow = f;
        ch[0].freq = f;
        update_freq(0);
        sweep_calc();
    }
}

void gbl_apu_set_rate(uint32_t hz) {
    rate = hz;
    uint64_t k = ((uint64_t)1 << 49) / hz;
    square_hi = (uint32_t)(k >> 16);
    square_lo = (uint16_t)k;
    k = ((uint64_t)1 << 48) / hz;
    wave_hi = (uint32_t)(k >> 16);
    wave_lo = (uint16_t)k;
    noise_num = (uint32_t)(((uint64_t)1 << 36) / (2u * hz));
    seq_step_samples = (hz << 8) / 512u; /* samples per sequencer step, 24.8 */
    for (int c = 0; c < 3; c++) update_freq(c);
    ch[3].inc = noise_inc(NR(0x22));
}

void gbl_apu_reset(void) {
    memset(ch, 0, sizeof(ch));
    sweep_timer = sweep_on = 0;
    lfsr = 0x7FFF;
    seq_acc = 0;
    seq_step = 0;
    /* DMG register state after the boot ROM; its chime has decayed to
     * silence but channel 1 still reports enabled. */
    static const uint8_t boot[0x17] = {
        0x80, 0xBF, 0xF3, 0xFF, 0xBF, 0x00, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF,
        0x9F, 0xFF, 0xBF, 0x00, 0xFF, 0x00, 0x00, 0xBF, 0x77, 0xF3, 0xF1,
    };
    memcpy(&NR(0x10), boot, sizeof(boot));
    ch[0].on = ch[0].dac = 1;
    ch[3].dac = 0;
    gbl_apu_set_rate(rate);
}

uint8_t gbl_apu_read(uint8_t n) {
    if (n >= 0x30) return gbl_io[n];
    if (n == 0x26) {
        uint8_t v = NR(0x26) & 0x80;
        for (int c = 0; c < 4; c++) if (ch[c].on) v |= (uint8_t)(1u << c);
        return v | 0x70;
    }
    return gbl_io[n] | read_mask[n - 0x10];
}

void gbl_apu_write(uint8_t n, uint8_t v) {
    if (n >= 0x30) { gbl_io[n] = v; return; }
    if (n == 0x26) {
        if (!(v & 0x80)) {
            /* Power off clears every register except wave RAM. */
            memset(&gbl_io[0x10], 0, 0x16);
            for (int c = 0; c < 4; c++) { ch[c].on = 0; ch[c].dac = 0; }
        }
        NR(0x26) = v & 0x80;
        return;
    }
    if (!(NR(0x26) & 0x80)) {
        /* DMG keeps length counters writable while powered off. */
        if (n == 0x11 || n == 0x16 || n == 0x1B || n == 0x20) {}
        else return;
    }
    gbl_io[n] = v;
    switch (n) {
    case 0x11: ch[0].length = 64 - (v & 63); break;
    case 0x16: ch[1].length = 64 - (v & 63); break;
    case 0x1B: ch[2].length = 256 - v; break;
    case 0x20: ch[3].length = 64 - (v & 63); break;
    case 0x12: case 0x17: case 0x21: {
        int c = n == 0x12 ? 0 : n == 0x17 ? 1 : 3;
        ch[c].dac = (v & 0xF8) != 0;
        if (!ch[c].dac) ch[c].on = 0;
        break;
    }
    case 0x1A:
        ch[2].dac = (v & 0x80) != 0;
        if (!ch[2].dac) ch[2].on = 0;
        break;
    case 0x13: case 0x18: case 0x1D: {
        int c = n == 0x13 ? 0 : n == 0x18 ? 1 : 2;
        ch[c].freq = (uint16_t)((ch[c].freq & 0x700) | v);
        update_freq(c);
        break;
    }
    case 0x14: case 0x19: case 0x1E: {
        int c = n == 0x14 ? 0 : n == 0x19 ? 1 : 2;
        ch[c].freq = (uint16_t)((ch[c].freq & 0xFF) | ((v & 7) << 8));
        update_freq(c);
        if (v & 0x80) trigger(c);
        break;
    }
    case 0x22: ch[3].inc = noise_inc(v); break;
    case 0x23: if (v & 0x80) trigger(3); break;
    default: break;
    }
}

void gbl_apu_render(uint8_t *out, unsigned n) {
    uint8_t nr51 = NR(0x25), nr50 = NR(0x24);
    uint32_t master = (((nr50 >> 4) & 7) + (nr50 & 7) + 2u); /* 2..16, L+R average x2 */
    uint8_t route = (uint8_t)((nr51 | (nr51 >> 4)) & 0x0F & ~gbl_apu_mute);
    int powered = (NR(0x26) & 0x80) != 0;

    for (unsigned i = 0; i < n; i++) {
        seq_acc += 256u;
        if (seq_acc >= seq_step_samples) {
            seq_acc -= seq_step_samples;
            if (!(seq_step & 1)) clock_length();
            if (seq_step == 2 || seq_step == 6) clock_sweep();
            if (seq_step == 7) clock_envelope();
            seq_step = (seq_step + 1) & 7;
        }
        int32_t mix = 0;
        if (powered) {
            if (ch[0].on && (route & 1)) {
                ch[0].phase += ch[0].inc;
                int bit = (duty_table[NR(0x11) >> 6] >> (7 - (ch[0].phase >> 29))) & 1;
                mix += bit ? ch[0].vol : -(int32_t)ch[0].vol;
            }
            if (ch[1].on && (route & 2)) {
                ch[1].phase += ch[1].inc;
                int bit = (duty_table[NR(0x16) >> 6] >> (7 - (ch[1].phase >> 29))) & 1;
                mix += bit ? ch[1].vol : -(int32_t)ch[1].vol;
            }
            if (ch[2].on && (route & 4)) {
                ch[2].phase += ch[2].inc;
                uint32_t idx = ch[2].phase >> 27;
                uint8_t b = gbl_io[0x30 + (idx >> 1)];
                int s = (idx & 1) ? (b & 15) : (b >> 4);
                int code = (NR(0x1C) >> 5) & 3;
                if (code) mix += ((s << 1) - 15) >> (code - 1);
            }
            if (ch[3].on && (route & 8)) {
                ch[3].phase += ch[3].inc;
                uint32_t steps = ch[3].phase >> 16;
                ch[3].phase &= 0xFFFF;
                if (steps > 64) steps = 64;
                lfsr = gbl_apu_lfsr_steps(lfsr, steps, NR(0x22) & 0x08);
                mix += (lfsr & 1) ? -(int32_t)ch[3].vol : ch[3].vol;
            }
        }
        /* mix is within +-60; master 2..16 -> about +-120 at full volume. */
        int32_t s = 128 + ((mix * (int32_t)master) >> 3);
        out[i] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
    }
}

/* Save states (gbl_state_io of gbl_core.c); the registers are in gbl_io. */
void gbl_apu_state_io(gbl_state_fn io, void *ctx, int load) {
    (void)load;
    io(ch, sizeof(ch), ctx);
    io(&sweep_timer, sizeof(sweep_timer), ctx);
    io(&sweep_on, sizeof(sweep_on), ctx);
    io(&sweep_shadow, sizeof(sweep_shadow), ctx);
    io(&lfsr, sizeof(lfsr), ctx);
    io(&seq_acc, sizeof(seq_acc), ctx);
    io(&seq_step, sizeof(seq_step), ctx);
}
