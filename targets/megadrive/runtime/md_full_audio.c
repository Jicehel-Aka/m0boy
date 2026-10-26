#include "md_full.h"
#include <math.h>
#include <string.h>

/* One clock for every sound part: the NTSC master clock. The 68000 and the
 * YM2612 run at master/7, the Z80 and the PSG at master/15. The YM2612 makes
 * one sample every 144 of its clocks, the PSG ticks every 16 of its own. */
enum {
    MASTER_HZ = 53693175, AUDIO_RATE = 44100,
    FRAME_MASTER = 127856 * 7, VINT_MASTER = 109312 * 7, VINT_LENGTH = 228 * 15,
    FM_MASTER = 144 * 7, PSG_MASTER = 16 * 15, PSG_AMP = 1800,
};
enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

static uint16_t logsin[256], exprom[256];
static void tables(void) {
    static int ready;
    if (ready) return;
    for (unsigned i = 0; i < 256; ++i) {
        logsin[i] = (uint16_t)lround(-log2(sin((i * 2 + 1) * 3.14159265358979323846 / 1024.0)) * 256.0);
        exprom[i] = (uint16_t)lround((pow(2.0, i / 256.0) - 1.0) * 1024.0);
    }
    ready = 1;
}

static const uint8_t detune[4][32] = {
    {0},
    {0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1, 2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7},
    {1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5, 5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16},
    {2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7, 8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22},
};
static const uint8_t eg_inc[17][8] = {
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1}, {1,1,1,2,1,1,1,2}, {1,2,1,2,1,2,1,2}, {1,2,2,2,1,2,2,2},
    {2,2,2,2,2,2,2,2}, {2,2,2,4,2,2,2,4}, {2,4,2,4,2,4,2,4}, {2,4,4,4,2,4,4,4},
    {4,4,4,4,4,4,4,4}, {4,4,4,8,4,4,4,8}, {4,8,4,8,4,8,4,8}, {4,8,8,8,4,8,8,8},
    {8,8,8,8,8,8,8,8},
};
static const uint8_t op_offset[4] = {0, 8, 4, 12};
static const uint8_t lfo_period[8] = {108, 77, 71, 67, 62, 44, 8, 5};
/* PM depth per PMS (0, 3.4 ... 80 cents) as a 16.16 fraction of F-number. */
static const uint16_t pm_depth[8] = {0, 129, 254, 379, 532, 761, 1532, 3109};

static uint64_t m68k_time(const MdFull *m) {
    return ((uint64_t)m->frames * 127856u + m->frame_cycles) * 7u;
}

/* ---- YM2612 ------------------------------------------------------------ */

static void ym_reset(MdYm2612 *y) {
    memset(y, 0, sizeof *y);
    for (unsigned b = 0; b < 2; ++b)
        for (unsigned i = 0; i < 3; ++i) y->reg[b][0xb4 + i] = 0xc0;
    for (unsigned c = 0; c < 6; ++c)
        for (unsigned o = 0; o < 4; ++o) { y->op[c][o].att = 1023; y->op[c][o].state = EG_RELEASE; }
}

static void ym_frequency(const MdYm2612 *y, unsigned ch, unsigned op, unsigned *fnum, unsigned *block) {
    static const int8_t special[4] = {1, 2, 0, -1};
    if (ch == 2 && (y->reg[0][0x27] & 0xc0u) && special[op] >= 0) {
        *fnum = y->special_fnum[special[op]]; *block = y->special_block[special[op]];
    } else { *fnum = y->fnum[ch]; *block = y->block[ch]; }
}
static unsigned keycode(unsigned fnum, unsigned block) {
    static const uint8_t note[16] = {0,0,0,0,0,0,0,1,2,3,3,3,3,3,3,3};
    return block << 2 | note[fnum >> 7];
}
static unsigned eg_rate(const MdYm2612 *y, unsigned ch, unsigned op, unsigned state) {
    const unsigned bank = ch / 3u, at = op_offset[op] + ch % 3u;
    unsigned r;
    switch (state) {
    case EG_ATTACK: r = y->reg[bank][0x50 + at] & 31u; break;
    case EG_DECAY: r = y->reg[bank][0x60 + at] & 31u; break;
    case EG_SUSTAIN: r = y->reg[bank][0x70 + at] & 31u; break;
    default: r = (y->reg[bank][0x80 + at] & 15u) * 2u + 1u; break;
    }
    if (!r) return 0;
    unsigned fnum, block;
    ym_frequency(y, ch, op, &fnum, &block);
    r = r * 2u + (keycode(fnum, block) >> (3u - (y->reg[bank][0x50 + at] >> 6)));
    return r > 63u ? 63u : r;
}

static uint8_t ssg_mode(const MdYm2612 *y, unsigned ch, unsigned op) {
    return y->reg[ch / 3u][0x90 + op_offset[op] + ch % 3u] & 15u;
}
static unsigned sustain_level(const MdYm2612 *y, unsigned ch, unsigned op) {
    const unsigned sl = y->reg[ch / 3u][0x80 + op_offset[op] + ch % 3u] >> 4;
    return (sl == 15u ? 31u : sl) << 5;
}

static void ym_key(MdYm2612 *y, unsigned ch, unsigned op, unsigned on) {
    MdFmOperator *o = &y->op[ch][op];
    if (on && !o->key) {
        o->phase = 0;
        o->ssg_invert = 0;
        if (eg_rate(y, ch, op, EG_ATTACK) >= 62u) { o->att = 0; o->state = EG_DECAY; }
        else o->state = EG_ATTACK;
    } else if (!on && o->key) {
        const uint8_t ssg = ssg_mode(y, ch, op);
        /* An inverted SSG-EG output becomes the level the release starts from. */
        if ((ssg & 8u) && (o->ssg_invert ^ ((ssg >> 2) & 1u))) o->att = (uint16_t)((0x200u - o->att) & 0x3ffu);
        o->ssg_invert = 0;
        o->state = EG_RELEASE;
    }
    o->key = (uint8_t)on;
}
static void ym_keys(MdYm2612 *y, unsigned ch) {
    const unsigned bits = y->reg_key[ch] | (ch == 2u ? y->csm_key : 0u);
    for (unsigned op = 0; op < 4; ++op) ym_key(y, ch, op, (bits >> op) & 1u);
}

static void ym_write(MdYm2612 *y, unsigned bank, uint8_t reg, uint8_t v) {
    if (bank && reg < 0x30u) return;
    if (reg == 0x27u) {
        const uint8_t old = y->reg[0][0x27];
        if ((v & 1u) && !(old & 1u)) y->timer_a = (uint16_t)(y->reg[0][0x24] << 2 | (y->reg[0][0x25] & 3u));
        if ((v & 2u) && !(old & 2u)) { y->timer_b = y->reg[0][0x26]; y->timer_b_sub = 0; }
        y->status &= (uint8_t)~((v >> 4) & 3u);
    }
    y->reg[bank][reg] = v;
    if (reg == 0x22u && !(v & 8u)) { y->lfo_step = 0; y->lfo_sub = 0; }
    else if (reg == 0x28u) {
        const unsigned ch = (v & 3u) + ((v & 4u) ? 3u : 0u);
        if ((v & 3u) != 3u) { y->reg_key[ch] = v >> 4; ym_keys(y, ch); }
    } else if (reg >= 0xa0u && reg <= 0xa2u) {
        const unsigned ch = bank * 3u + (reg & 3u);
        y->fnum[ch] = (uint16_t)((y->fnum_latch & 7u) << 8 | v);
        y->block[ch] = (y->fnum_latch >> 3) & 7u;
    } else if (reg >= 0xa4u && reg <= 0xa6u) y->fnum_latch = v & 0x3fu;
    else if (!bank && reg >= 0xa8u && reg <= 0xaau) {
        y->special_fnum[reg - 0xa8u] = (uint16_t)((y->special_latch & 7u) << 8 | v);
        y->special_block[reg - 0xa8u] = (y->special_latch >> 3) & 7u;
    } else if (!bank && reg >= 0xacu && reg <= 0xaeu) y->special_latch = v & 0x3fu;
}

static void ym_envelopes(MdYm2612 *y) {
    if (++y->eg_timer < 3u) return;
    y->eg_timer = 0;
    if (++y->eg_cnt == 4096u) y->eg_cnt = 1;
    for (unsigned ch = 0; ch < 6; ++ch)
        for (unsigned op = 0; op < 4; ++op) {
            MdFmOperator *o = &y->op[ch][op];
            const unsigned rate = eg_rate(y, ch, op, o->state);
            if (!rate) continue;
            const unsigned shift = rate < 48u ? 11u - (rate >> 2) : 0u;
            if (y->eg_cnt & ((1u << shift) - 1u)) continue;
            const unsigned row = rate < 48u ? (rate & 3u) : rate < 60u ? rate - 44u : 16u;
            const unsigned inc = eg_inc[row][(y->eg_cnt >> shift) & 7u];
            int att = o->att;
            if (o->state == EG_ATTACK) {
                att -= ((att + 1) * (int)inc + 15) >> 4;
                if (att <= 0) { att = 0; o->state = EG_DECAY; }
            } else if (ssg_mode(y, ch, op) & 8u) {
                /* SSG-EG: four times faster, up to 0x200 only. */
                if (att < 0x200) att += 4 * (int)inc;
                if (o->state == EG_RELEASE && att >= 0x200) att = 1023;
            } else {
                att += (int)inc;
                if (att > 1023) att = 1023;
            }
            if (o->state == EG_DECAY && att >= (int)sustain_level(y, ch, op)) o->state = EG_SUSTAIN;
            o->att = (uint16_t)att;
        }
}

/* SSG-EG at the end of each cycle (0x200): repeat, alternate or hold. */
static void ym_ssg(MdYm2612 *y) {
    for (unsigned ch = 0; ch < 6; ++ch)
        for (unsigned op = 0; op < 4; ++op) {
            MdFmOperator *o = &y->op[ch][op];
            const uint8_t ssg = ssg_mode(y, ch, op);
            if (!(ssg & 8u) || o->att < 0x200u || o->state == EG_RELEASE) continue;
            if (ssg & 1u) {
                if (ssg & 2u) o->ssg_invert = 1;
                if (o->state != EG_ATTACK && !(o->ssg_invert ^ ((ssg >> 2) & 1u))) o->att = 1023;
            } else {
                if (ssg & 2u) o->ssg_invert ^= 1u;
                else o->phase = 0;
                if (o->state != EG_ATTACK) {
                    const unsigned sl = sustain_level(y, ch, op);
                    if (eg_rate(y, ch, op, EG_ATTACK) < 62u) o->state = EG_ATTACK;
                    else { o->att = 0; o->state = sl ? EG_DECAY : EG_SUSTAIN; }
                }
            }
        }
}

static void ym_timers(MdYm2612 *y) {
    const uint8_t control = y->reg[0][0x27];
    if (y->csm_key) { y->csm_key = 0; ym_keys(y, 2); }
    if ((control & 1u) && ++y->timer_a >= 1024u) {
        y->timer_a = (uint16_t)(y->reg[0][0x24] << 2 | (y->reg[0][0x25] & 3u));
        if (control & 4u) y->status |= 1u;
        /* CSM: each overflow keys channel 3 on for one sample. */
        if ((control & 0xc0u) == 0x80u) { y->csm_key = 15; ym_keys(y, 2); }
    }
    if ((control & 2u) && ++y->timer_b_sub >= 16u) {
        y->timer_b_sub = 0;
        if (++y->timer_b >= 256u) {
            y->timer_b = y->reg[0][0x26];
            if (control & 8u) y->status |= 2u;
        }
    }
}

static int ym_operator(MdYm2612 *y, unsigned ch, unsigned op, int mod, unsigned am, int pm) {
    MdFmOperator *o = &y->op[ch][op];
    const unsigned bank = ch / 3u, at = op_offset[op] + ch % 3u;
    const uint8_t dtmul = y->reg[bank][0x30 + at];
    unsigned fnum, block;
    ym_frequency(y, ch, op, &fnum, &block);
    if (pm) fnum = (unsigned)((int)fnum + (int)fnum * pm / (65536 * 32)) & 0x7ffu;
    const unsigned kc = keycode(fnum, block), dt = (dtmul >> 4) & 7u, mul = dtmul & 15u;
    int base = (int)((fnum << block) >> 1);
    base += (dt & 4u) ? -(int)detune[dt & 3u][kc] : detune[dt & 3u][kc];
    const uint32_t inc = (uint32_t)base & 0x1ffffu;
    const uint32_t phase = o->phase;
    o->phase = (phase + (mul ? inc * mul : inc >> 1)) & 0xfffffu;

    const uint8_t ssg = y->reg[bank][0x90 + at];
    unsigned level = (ssg & 8u) && (o->ssg_invert ^ ((ssg >> 2) & 1u)) ? (0x200u - o->att) & 0x3ffu : o->att;
    level += (y->reg[bank][0x40 + at] & 127u) << 3;
    if (y->reg[bank][0x60 + at] & 0x80u) level += am;
    if (level > 1023u) level = 1023u;
    const unsigned p = ((phase >> 10) + (unsigned)mod) & 1023u;
    level = logsin[(p & 0x100u) ? (~p & 255u) : (p & 255u)] + (level << 2);
    int out = level >> 8 >= 13u ? 0 : ((exprom[(level & 255u) ^ 255u] | 0x400) << 2) >> (level >> 8);
    if (p & 0x200u) out = -out;
    o->prev = o->out;
    o->out = (int16_t)out;
    return out;
}

static void ym_sample(MdYm2612 *y, int *left, int *right) {
    unsigned lfo_am = 0;
    int lfo_pm = 0;
    if (y->reg[0][0x22] & 8u) {
        if (++y->lfo_sub >= lfo_period[y->reg[0][0x22] & 7u]) { y->lfo_sub = 0; y->lfo_step = (y->lfo_step + 1u) & 127u; }
        lfo_am = y->lfo_step < 64u ? y->lfo_step * 2u : (127u - y->lfo_step) * 2u;
        const int t = y->lfo_step & 63u;   /* triangle, -32..32 */
        lfo_pm = (y->lfo_step < 64u ? 1 : -1) * (t < 32 ? t : 64 - t);
    }
    ym_timers(y);
    ym_envelopes(y);
    ym_ssg(y);
    *left = *right = 0;
    for (unsigned ch = 0; ch < 6; ++ch) {
        const unsigned bank = ch / 3u, lane = ch % 3u;
        const uint8_t b0 = y->reg[bank][0xb0 + lane], b4 = y->reg[bank][0xb4 + lane];
        static const uint8_t am_shift[4] = {8, 3, 1, 0};
        const unsigned am = lfo_am >> am_shift[(b4 >> 4) & 3u];
        const int pm = lfo_pm * pm_depth[b4 & 7u];
        const unsigned fb = (b0 >> 3) & 7u;
        const MdFmOperator *s1 = &y->op[ch][0];
        const int feedback = fb ? (s1->out + s1->prev) >> (10u - fb) : 0;
        const int a = ym_operator(y, ch, 0, feedback, am, pm);
        int b, c, d, out;
        switch (b0 & 7u) {
        case 0: b = ym_operator(y, ch, 1, a >> 1, am, pm); c = ym_operator(y, ch, 2, b >> 1, am, pm);
                out = ym_operator(y, ch, 3, c >> 1, am, pm); break;
        case 1: b = ym_operator(y, ch, 1, 0, am, pm); c = ym_operator(y, ch, 2, (a + b) >> 1, am, pm);
                out = ym_operator(y, ch, 3, c >> 1, am, pm); break;
        case 2: b = ym_operator(y, ch, 1, 0, am, pm); c = ym_operator(y, ch, 2, b >> 1, am, pm);
                out = ym_operator(y, ch, 3, (a + c) >> 1, am, pm); break;
        case 3: b = ym_operator(y, ch, 1, a >> 1, am, pm); c = ym_operator(y, ch, 2, 0, am, pm);
                out = ym_operator(y, ch, 3, (b + c) >> 1, am, pm); break;
        case 4: b = ym_operator(y, ch, 1, a >> 1, am, pm); c = ym_operator(y, ch, 2, 0, am, pm);
                out = b + ym_operator(y, ch, 3, c >> 1, am, pm); break;
        case 5: b = ym_operator(y, ch, 1, a >> 1, am, pm); c = ym_operator(y, ch, 2, a >> 1, am, pm);
                out = b + c + ym_operator(y, ch, 3, a >> 1, am, pm); break;
        case 6: b = ym_operator(y, ch, 1, a >> 1, am, pm); c = ym_operator(y, ch, 2, 0, am, pm);
                out = b + c + ym_operator(y, ch, 3, 0, am, pm); break;
        default: b = ym_operator(y, ch, 1, 0, am, pm); c = ym_operator(y, ch, 2, 0, am, pm);
                 d = ym_operator(y, ch, 3, 0, am, pm); out = a + b + c + d; break;
        }
        if (ch == 5 && (y->reg[0][0x2b] & 0x80u)) out = ((int)y->reg[0][0x2a] - 128) << 6;
        if (out > 8191) out = 8191;
        if (out < -8192) out = -8192;
        /* 9-bit DAC of the discrete YM2612: non-negative levels gain one step
         * and a muted side still outputs the sign (the "ladder effect"). */
        int level = out >> 5, sign = level < 0 ? -1 : 1;
        if (level >= 0) ++level;
        *left += ((b4 & 0x80u) ? level : sign) * 32;
        *right += ((b4 & 0x40u) ? level : sign) * 32;
    }
}

/* ---- SN76489 (Sega variant) ------------------------------------------- */

static void psg_write(MdPsg *p, uint8_t value) {
    if (value & 0x80u) p->latch = (value >> 4) & 7u;
    const unsigned r = p->latch, ch = r >> 1;
    if (r & 1u) p->volume[ch] = value & 15u;
    else if (ch == 3u) { p->noise = value & 7u; p->lfsr = 0x8000u; }
    else if (value & 0x80u) p->tone[ch] = (uint16_t)((p->tone[ch] & 0x3f0u) | (value & 15u));
    else p->tone[ch] = (uint16_t)((p->tone[ch] & 15u) | ((value & 63u) << 4));
}

static void psg_tick(MdPsg *p) {
    for (unsigned ch = 0; ch < 3; ++ch) {
        if (p->tone[ch] <= 1u) { p->high[ch] = 1; continue; }   /* sample playback level */
        if (p->counter[ch] <= 1u) { p->counter[ch] = p->tone[ch]; p->high[ch] ^= 1u; }
        else --p->counter[ch];
    }
    if (p->counter[3] <= 1u) {
        const unsigned rate = p->noise & 3u;
        p->counter[3] = rate == 3u ? (p->tone[2] ? p->tone[2] : 1u) : 0x10u << rate;
        p->high[3] ^= 1u;
        if (p->high[3]) {
            const unsigned bit = (p->noise & 4u) ? ((p->lfsr ^ (p->lfsr >> 3)) & 1u) : (p->lfsr & 1u);
            p->lfsr = (uint16_t)((p->lfsr >> 1) | (bit << 15));
        }
    } else --p->counter[3];
}

static int psg_output(const MdPsg *p) {
    static int amp[16];
    if (!amp[0]) for (unsigned i = 0; i < 15; ++i) amp[i] = (int)lround(PSG_AMP * pow(10.0, -0.1 * i));
    int mix = 0;
    for (unsigned ch = 0; ch < 3; ++ch) mix += p->high[ch] ? amp[p->volume[ch]] : -amp[p->volume[ch]];
    return mix + ((p->lfsr & 1u) ? amp[p->volume[3]] : -amp[p->volume[3]]);
}

/* ---- Mixing and resampling -------------------------------------------- */

static void audio_emit(MdFull *m, int left, int right) {
    const int in[2] = {left, right};
    int16_t s[2];
    for (unsigned c = 0; c < 2; ++c) {
        /* Remove DC (about 3 Hz), like the console's output capacitors. */
        m->audio_dc[c] += ((in[c] * 256) - m->audio_dc[c]) / 2048;
        const int32_t y = in[c] - m->audio_dc[c] / 256;
        s[c] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        m->audio_hash = (m->audio_hash ^ (uint8_t)s[c]) * 16777619u;
        m->audio_hash = (m->audio_hash ^ (uint8_t)((uint16_t)s[c] >> 8)) * 16777619u;
    }
    if (m->audio_frames < sizeof m->audio_out / sizeof m->audio_out[0] / 2u) {
        m->audio_out[m->audio_frames * 2u] = s[0];
        m->audio_out[m->audio_frames * 2u + 1u] = s[1];
        ++m->audio_frames;
    }
}

/* Produces every FM sample that ends at or before master clock `until`. */
static void audio_run(MdFull *m, uint64_t until) {
    while (m->audio_time + FM_MASTER <= until) {
        int fm[2];
        ym_sample(&m->ym, &fm[0], &fm[1]);
        int psg = 0, ticks = 0;
        for (m->psg_rest += FM_MASTER; m->psg_rest >= PSG_MASTER; m->psg_rest -= PSG_MASTER, ++ticks) {
            psg_tick(&m->psg);
            psg += psg_output(&m->psg);
        }
        if (ticks) psg /= ticks;
        const int now[2] = {fm[0] * 3 / 4 + psg, fm[1] * 3 / 4 + psg};
        /* Output frame k sits at k * MASTER_HZ / AUDIO_RATE: emit those that
         * fall inside this FM sample, interpolating from the previous one. */
        const uint64_t start = m->audio_time * AUDIO_RATE, end = (m->audio_time + FM_MASTER) * AUDIO_RATE;
        uint64_t k = (start + MASTER_HZ) / MASTER_HZ;   /* first k with k*MASTER_HZ > start */
        for (; k * MASTER_HZ <= end; ++k) {
            const int64_t f = (int64_t)(k * MASTER_HZ - start), span = (int64_t)(end - start);
            audio_emit(m, (int)(m->audio_last[0] + (now[0] - m->audio_last[0]) * f / span),
                          (int)(m->audio_last[1] + (now[1] - m->audio_last[1]) * f / span));
        }
        m->audio_last[0] = now[0];
        m->audio_last[1] = now[1];
        m->audio_time += FM_MASTER;
    }
}

/* ---- Z80 and its bus --------------------------------------------------- */

static uint64_t z80_time(const MdFull *m) { return m->sound_cpu.cycles * 15u; }

static void z80_run(MdFull *m, uint64_t until) {
    z80_t *z = &m->sound_cpu;
    const uint64_t target = until / 15u;
    if (!m->z80_reset || m->z80_bus || z->fault) {
        if (z->cycles < target) z->cycles = target;
        return;
    }
    while (z->cycles < target && !z->fault) {
        /* /INT is held for about one line from the start of vertical blank. */
        const uint64_t at = z80_time(m) % FRAME_MASTER;
        z->irq = at >= VINT_MASTER && at < VINT_MASTER + VINT_LENGTH;
        z80_step(z);
    }
    z->irq = 0;
}

static void sound_store(MdFull *m, uint16_t addr, uint8_t value, uint64_t at) {
    if (addr < 0x4000u) { m->zram[addr & 0x1fffu] = value; return; }
    if (addr < 0x6000u) {
        audio_run(m, at);
        const unsigned bank = (addr >> 1) & 1u;
        if (!(addr & 1u)) m->ym.addr[bank] = value;
        else {
            ym_write(&m->ym, bank, m->ym.addr[bank], value);
            m->ym.busy_until = at + 32u * 6u * 7u;   /* 32 internal clocks of fM/6 */
        }
        return;
    }
    if (addr < 0x6100u) {
        m->z80_bank = ((m->z80_bank >> 1) | ((uint32_t)(value & 1u) << 23)) & 0xff8000u;
        return;
    }
    if ((addr & 0xfff9u) == 0x7f11u) { audio_run(m, at); psg_write(&m->psg, value); }
}

static uint8_t sound_load(MdFull *m, uint16_t addr, uint64_t at) {
    if (addr < 0x4000u) return m->zram[addr & 0x1fffu];
    if (addr < 0x6000u) { audio_run(m, at); return (uint8_t)(m->ym.status | (at < m->ym.busy_until ? 0x80u : 0u)); }
    return 0xff;
}

static uint8_t zread(void *user, uint16_t addr) {
    MdFull *m = (MdFull *)user;
    if (addr >= 0x8000u) {
        const uint32_t a = (m->z80_bank | (addr & 0x7fffu)) & 0xffffffu;
        if (a < md_rom_size) return md_rom[a];
        if (a >= 0xff0000u) return m->ram[a & 0xffffu];
        return 0xff;
    }
    return sound_load(m, addr, z80_time(m));
}
static void zwrite(void *user, uint16_t addr, uint8_t value) {
    MdFull *m = (MdFull *)user;
    if (addr >= 0x8000u) {
        const uint32_t a = (m->z80_bank | (addr & 0x7fffu)) & 0xffffffu;
        if (a >= 0xff0000u) m->ram[a & 0xffffu] = value;
        else if ((a & 0xffffe0u) == 0xc00000u && (a & 0x19u) == 0x11u) sound_store(m, 0x7f11u, value, z80_time(m));
        return;
    }
    sound_store(m, addr, value, z80_time(m));
}
static uint8_t zin(void *user, uint16_t port) { (void)user; (void)port; return 0xff; }
static void zout(void *user, uint16_t port, uint8_t value) { (void)user; (void)port; (void)value; }

void md_full_audio_reset(MdFull *m) {
    tables();
    m->sound_cpu.user = m;
    m->sound_cpu.read = zread;
    m->sound_cpu.write = zwrite;
    m->sound_cpu.in = zin;
    m->sound_cpu.out = zout;
    z80_reset(&m->sound_cpu);
    m->sound_cpu.cycles = 0;
    m->z80_reset = 0;
    ym_reset(&m->ym);
    memset(&m->psg, 0, sizeof m->psg);
    m->psg.lfsr = 0x8000u;
    for (unsigned i = 0; i < 4; ++i) m->psg.volume[i] = 15;
    m->audio_time = 0;
    m->audio_frames = 0;
    m->audio_hash = 2166136261u;
}

/* 68000 side: the Z80 and the sound chips first catch up with its clock. */
void md_full_sound_write(MdFull *m, uint32_t addr, uint8_t value) {
    const uint64_t now = m68k_time(m);
    z80_run(m, now);
    sound_store(m, (uint16_t)addr, value, now);
}
uint8_t md_full_sound_read(MdFull *m, uint32_t addr) {
    const uint64_t now = m68k_time(m);
    z80_run(m, now);
    return sound_load(m, (uint16_t)addr, now);
}

/* reset_line 0: bus request (A11100), 1: reset (A11200, 1 = running). */
void md_full_sound_control(MdFull *m, unsigned reset_line, uint8_t value) {
    const uint64_t now = m68k_time(m);
    z80_run(m, now);
    if (!reset_line) { m->z80_bus = value; return; }
    if (value && !m->z80_reset) {
        const uint64_t cycles = m->sound_cpu.cycles;
        z80_reset(&m->sound_cpu);
        m->sound_cpu.cycles = cycles;
    } else if (!value && m->z80_reset) {
        /* The YM2612 shares the Z80 reset line. */
        audio_run(m, now);
        ym_reset(&m->ym);
    }
    m->z80_reset = value;
}

void md_full_sound_frame(MdFull *m) {
    const uint64_t end = (uint64_t)m->frames * FRAME_MASTER;
    z80_run(m, end);
    audio_run(m, end);
}

unsigned md_full_audio_read(MdFull *m, int16_t *samples, unsigned max) {
    const unsigned n = m->audio_frames < max ? m->audio_frames : max;
    memcpy(samples, m->audio_out, n * 2u * sizeof *samples);
    memmove(m->audio_out, m->audio_out + n * 2u, (m->audio_frames - n) * 2u * sizeof *samples);
    m->audio_frames -= n;
    return n;
}
