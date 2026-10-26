/* YM2612 (OPN2), one sample per 144 clocks.  Phase, envelope, SSG-EG and
 * LFO follow the chip as the Nuked-OPN2 analysis and ymfm describe it;
 * tests/md_ym_compare.cpp plays a game's writes through this and ymfm and
 * checks that the two outputs agree.
 *
 * The sample loop works on values prepared by md_ym_write (envelope rates,
 * levels, detune, multiplier) and leaves silent operators alone: an
 * operator with no key and past the silence threshold keeps only its
 * envelope, up to the 0x3ff floor, until a key-on brings it back.  Both are
 * invisible to the output, which the ymfm comparison pins down. */
#include "md_ym2612.h"
#include <math.h>
#include <string.h>

enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE, EG_QUIET = 0x380, EG_FLOOR = 0x3ff };

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

/* Detune per key code, DT 1..3 (DT 0 is none; bit 2 negates). */
static const uint8_t detune[32][3] = {
    {0,1,2}, {0,1,2}, {0,1,2}, {0,1,2}, {1,2,2}, {1,2,3}, {1,2,3}, {1,2,3},
    {1,2,4}, {1,3,4}, {1,3,4}, {1,3,5}, {2,4,5}, {2,4,6}, {2,4,6}, {2,5,7},
    {2,5,8}, {3,6,8}, {3,6,9}, {3,7,10}, {4,8,11}, {4,8,12}, {4,9,13}, {5,10,14},
    {5,11,16}, {6,12,17}, {6,13,19}, {7,14,20}, {8,16,22}, {8,16,22}, {8,16,22}, {8,16,22},
};
/* Envelope increments: eight 4-bit steps per rate, step 0 in the low nibble. */
static const uint32_t eg_increment[64] = {
    0x00000000, 0x00000000, 0x10101010, 0x10101010, 0x10101010, 0x10101010, 0x11101110, 0x11101110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x11111111, 0x21112111, 0x21212121, 0x22212221, 0x22222222, 0x42224222, 0x42424242, 0x44424442,
    0x44444444, 0x84448444, 0x84848484, 0x88848884, 0x88888888, 0x88888888, 0x88888888, 0x88888888,
};
/* LFO PM: two right shifts of F-number bits 4..10 per PMS and |PM| step. */
static const uint8_t pm_shifts[8][8] = {
    {0x77,0x77,0x77,0x77,0x77,0x77,0x77,0x77}, {0x77,0x77,0x77,0x77,0x72,0x72,0x72,0x72},
    {0x77,0x77,0x77,0x72,0x72,0x72,0x17,0x17}, {0x77,0x77,0x72,0x72,0x17,0x17,0x12,0x12},
    {0x77,0x77,0x72,0x17,0x17,0x17,0x12,0x07}, {0x77,0x77,0x17,0x12,0x07,0x07,0x02,0x01},
    {0x77,0x77,0x17,0x12,0x07,0x07,0x02,0x01}, {0x77,0x77,0x17,0x12,0x07,0x07,0x02,0x01},
};
static const uint8_t lfo_period[8] = {109, 78, 72, 68, 63, 45, 9, 6};
static const uint8_t op_offset[4] = {0, 8, 4, 12};
/* Operator of each lane inside a 16-byte register block (S1, S3, S2, S4);
 * lane 3 is not a register. */
static const int8_t op_of_lane[16] = {0,0,0,-1, 2,2,2,-1, 1,1,1,-1, 3,3,3,-1};
/* Channel 3's special mode gives S1..S3 their own F-numbers (A9: S1, AA: S2,
 * A8: S3); S4 keeps the channel's. */
static const int8_t special_fnum_of_op[4] = {1, 2, 0, -1};

static unsigned reg_of(const MdYm2612 *y, unsigned base, unsigned ch, unsigned op) {
    return y->reg[ch / 3u][base + op_offset[op] + ch % 3u];
}

static void ym_prepare_op(MdYm2612 *y, unsigned ch, unsigned op);

void md_ym_reset(MdYm2612 *y) {
    tables();
    memset(y, 0, sizeof *y);
    for (unsigned b = 0; b < 2; ++b)
        for (unsigned i = 0; i < 3; ++i) y->reg[b][0xb4 + i] = 0xc0;
    y->reg[0][0x2a] = 0x80;   /* the DAC starts centred */
    for (unsigned c = 0; c < 6; ++c)
        for (unsigned o = 0; o < 4; ++o) { y->op[c][o].att = 1023; y->op[c][o].state = EG_RELEASE; }
    for (unsigned c = 0; c < 6; ++c)
        for (unsigned o = 0; o < 4; ++o) ym_prepare_op(y, c, o);
}

/* Block and F-number of an operator: channel 3's S1..S3 have their own in
 * the special mode. */
static unsigned block_fnum(const MdYm2612 *y, unsigned ch, unsigned op) {
    if (ch == 2 && (y->reg[0][0x27] & 0xc0u) && special_fnum_of_op[op] >= 0)
        return (unsigned)y->special_block[special_fnum_of_op[op]] << 11 | y->special_fnum[special_fnum_of_op[op]];
    return (unsigned)y->block[ch] << 11 | y->fnum[ch];
}
static unsigned keycode(unsigned bf) {
    return (bf >> 10) << 1 | ((0xfe80u >> ((bf >> 7) & 15u)) & 1u);
}
static unsigned eg_rate(const MdYm2612 *y, unsigned ch, unsigned op, unsigned state) {
    unsigned r;
    switch (state) {
    case EG_ATTACK: r = (reg_of(y, 0x50, ch, op) & 31u) * 2u; break;
    case EG_DECAY: r = (reg_of(y, 0x60, ch, op) & 31u) * 2u; break;
    case EG_SUSTAIN: r = (reg_of(y, 0x70, ch, op) & 31u) * 2u; break;
    default: r = (reg_of(y, 0x80, ch, op) & 15u) * 4u + 2u; break;
    }
    if (!r) return 0;
    r += keycode(block_fnum(y, ch, op)) >> ((reg_of(y, 0x50, ch, op) >> 6) ^ 3u);
    return r > 63u ? 63u : r;
}
static unsigned sustain_level(const MdYm2612 *y, unsigned ch, unsigned op) {
    unsigned sl = reg_of(y, 0x80, ch, op) >> 4;
    return (sl | ((sl + 1u) & 0x10u)) << 5;
}

/* Everything the sample loop reads per operator, derived from the registers
 * and from the block/F-num they scale with.  Any write that reaches an
 * operator register or changes a block/F-num comes through here. */
static void ym_prepare_op(MdYm2612 *y, unsigned ch, unsigned op) {
    MdFmOperator *o = &y->op[ch][op];
    o->tl8 = (uint16_t)((reg_of(y, 0x40, ch, op) & 127u) << 3);
    o->am_on = (uint8_t)(reg_of(y, 0x60, ch, op) >> 7 & 1u);
    o->ssg = reg_of(y, 0x90, ch, op);
    o->sl = (uint16_t)sustain_level(y, ch, op);
    o->rate[EG_ATTACK] = (uint8_t)eg_rate(y, ch, op, EG_ATTACK);
    o->rate[EG_DECAY] = (uint8_t)eg_rate(y, ch, op, EG_DECAY);
    o->rate[EG_SUSTAIN] = (uint8_t)eg_rate(y, ch, op, EG_SUSTAIN);
    o->rate[EG_RELEASE] = (uint8_t)eg_rate(y, ch, op, EG_RELEASE);
    const unsigned dtmul = reg_of(y, 0x30, ch, op), dt = (dtmul >> 4) & 7u;
    o->dt_idx = (uint8_t)(dt & 3u);
    o->dt_neg = (uint8_t)(dt & 4u);
    o->mul2 = (uint16_t)((dtmul & 15u) ? (dtmul & 15u) * 2u : 1u);
    o->det_d = o->dt_idx ? detune[keycode(block_fnum(y, ch, op))][o->dt_idx - 1u] : 0;
}
static void ym_prepare_channel(MdYm2612 *y, unsigned ch) {
    for (unsigned op = 0; op < 4; ++op) ym_prepare_op(y, ch, op);
}

static void start_attack(MdFmOperator *o, int restart) {
    if (o->state == EG_ATTACK) return;
    o->state = EG_ATTACK;
    if (!restart) {
        o->ssg_invert = (o->ssg & 8u) && (o->ssg & 4u);
        o->phase = 0;
    }
    if (o->rate[EG_ATTACK] >= 62u) o->att = 0;
}

static void ym_key(MdYm2612 *y, unsigned ch, unsigned op, unsigned on) {
    MdFmOperator *o = &y->op[ch][op];
    if (on == o->key) return;
    o->key = (uint8_t)on;
    if (on) start_attack(o, 0);
    else if (o->state != EG_RELEASE) {
        o->state = EG_RELEASE;
        /* An inverted SSG-EG output becomes the level the release starts from. */
        if (o->ssg_invert) { o->att = (uint16_t)((0x200u - o->att) & 0x3ffu); o->ssg_invert = 0; }
    }
}
static void ym_keys(MdYm2612 *y, unsigned ch) {
    const unsigned bits = y->reg_key[ch] | (ch == 2u ? y->csm_key : 0u);
    for (unsigned op = 0; op < 4; ++op) ym_key(y, ch, op, (bits >> op) & 1u);
}

void md_ym_write(MdYm2612 *y, unsigned bank, uint8_t reg, uint8_t v) {
    if (bank && reg < 0x30u) return;
    if (reg == 0x27u) {
        const uint8_t old = y->reg[0][0x27];
        if ((v & 1u) && !(old & 1u)) y->timer_a = (uint16_t)(y->reg[0][0x24] << 2 | (y->reg[0][0x25] & 3u));
        if ((v & 2u) && !(old & 2u)) { y->timer_b = y->reg[0][0x26]; y->timer_b_sub = 0; }
        y->status &= (uint8_t)~((v >> 4) & 3u);
    }
    y->reg[bank][reg] = v;
    if (reg == 0x28u) {
        const unsigned ch = (v & 3u) + ((v & 4u) ? 3u : 0u);
        if ((v & 3u) != 3u) y->reg_key[ch] = v >> 4;   /* sampled at the next sample */
    } else if (reg >= 0xa0u && reg <= 0xa2u) {
        const unsigned ch = bank * 3u + (reg & 3u);
        y->fnum[ch] = (uint16_t)((y->fnum_latch & 7u) << 8 | v);
        y->block[ch] = (y->fnum_latch >> 3) & 7u;
        ym_prepare_channel(y, ch);
    } else if (reg >= 0xa4u && reg <= 0xa6u) y->fnum_latch = v & 0x3fu;
    else if (!bank && reg >= 0xa8u && reg <= 0xaau) {
        y->special_fnum[reg - 0xa8u] = (uint16_t)((y->special_latch & 7u) << 8 | v);
        y->special_block[reg - 0xa8u] = (y->special_latch >> 3) & 7u;
        ym_prepare_channel(y, 2);
    } else if (!bank && reg >= 0xacu && reg <= 0xaeu) y->special_latch = v & 0x3fu;
    else if (reg == 0x27u) ym_prepare_channel(y, 2);   /* special mode on or off */
    else if (reg >= 0x30u) {
        const int op = op_of_lane[reg & 15u];
        if (op >= 0) ym_prepare_op(y, bank * 3u + (reg & 3u), (unsigned)op);
    }
}

/* SSG-EG, once the attenuation has crossed 0x200: repeat, alternate, hold. */
static void ssg_step(MdFmOperator *o) {
    const unsigned ssg = o->ssg;
    if (!(ssg & 8u)) { o->ssg_invert = 0; return; }
    if (!(o->att & 0x200u)) return;
    if (ssg & 1u) {
        o->ssg_invert = ((ssg >> 2) ^ (ssg >> 1)) & 1u;
        if (o->state != EG_ATTACK) o->att = o->ssg_invert ? 0x200 : 0x3ff;
    } else {
        o->ssg_invert ^= (ssg >> 1) & 1u;
        if (o->state == EG_DECAY || o->state == EG_SUSTAIN) start_attack(o, 1);
        if (!(ssg & 2u)) o->phase = 0;
    }
    if (o->state == EG_RELEASE) o->att = 0x3ff;
}

static void envelope_step(MdFmOperator *o, uint32_t counter) {
    if (o->state == EG_ATTACK && o->att == 0) o->state = EG_DECAY;
    if (o->state == EG_DECAY && o->att >= o->sl) o->state = EG_SUSTAIN;
    const unsigned rate = o->rate[o->state], shift = rate >> 2;
    counter <<= shift;
    if (counter & 0x7ffu) return;
    const unsigned inc = (eg_increment[rate] >> (4u * ((counter >> (shift <= 11u ? 11u : shift)) & 7u))) & 15u;
    if (o->state == EG_ATTACK) {
        if (rate < 62u) o->att = (uint16_t)(o->att + ((~(int)o->att * (int)inc) >> 4));
        return;
    }
    unsigned att = o->att;
    if (!(o->ssg & 8u)) att += inc;
    else if (att < 0x200u) att += 4u * inc;
    o->att = (uint16_t)(att >= 0x400u ? 0x3ffu : att);
}

/* LFO-adjusted, block-scaled phase step of one block/F-num, before detune:
 * every operator sharing the F-num shares this. */
static unsigned phase_base(const MdYm2612 *y, unsigned ch, unsigned bf, int lfo_pm) {
    unsigned fnum = (bf & 0x7ffu) << 1;
    const unsigned pms = y->reg[ch / 3u][0xb4 + ch % 3u] & 7u;
    if (pms && (y->reg[0][0x22] & 8u)) {
        const unsigned bits = (bf >> 4) & 0x7fu, shifts = pm_shifts[pms][(lfo_pm < 0 ? -lfo_pm : lfo_pm) & 7];
        int adjust = (int)((bits >> (shifts & 15u)) + (bits >> (shifts >> 4)));
        if (pms > 5u) adjust <<= pms - 5u;
        adjust >>= 2;
        fnum = (unsigned)((int)fnum + (lfo_pm < 0 ? -adjust : adjust)) & 0xfffu;
    }
    return (fnum << (bf >> 11)) >> 2;
}
static void phase_advance(MdFmOperator *o, unsigned base) {
    unsigned step = base;
    if (o->dt_idx) step = o->dt_neg ? step - o->det_d : step + o->det_d;
    o->phase += ((step & 0x1ffffu) * o->mul2) >> 1;
}

static int op_volume(const MdFmOperator *o, int mod, unsigned am) {
    if (o->att > EG_QUIET) return 0;
    unsigned level = o->ssg_invert ? (0x200u - o->att) & 0x3ffu : o->att;
    if (o->am_on) level += am;
    level += o->tl8;
    if (level > 0x3ffu) level = 0x3ffu;
    const unsigned p = ((o->phase >> 10) + (unsigned)mod) & 1023u;
    level = logsin[(p & 0x100u) ? (~p & 255u) : (p & 255u)] + (level << 2);
    const int out = level >> 8 >= 13u ? 0 : ((exprom[(level & 255u) ^ 255u] | 0x400) << 2) >> (level >> 8);
    return (p & 0x200u) ? -out : out;
}

/* Silent and unable to come back without a key-on (SSG-EG excluded, it
 * recycles the envelope on its own). */
static int op_silent(const MdFmOperator *o) {
    return !o->key && o->att > EG_QUIET && !(o->ssg & 8u);
}

static void ym_timers(MdYm2612 *y) {
    const uint8_t control = y->reg[0][0x27];
    if ((control & 1u) && ++y->timer_a >= 1024u) {
        y->timer_a = (uint16_t)(y->reg[0][0x24] << 2 | (y->reg[0][0x25] & 3u));
        if (control & 4u) y->status |= 1u;
        /* CSM: each overflow keys channel 3 on for one sample. */
        if ((control & 0xc0u) == 0x80u) y->csm_key = 15;
    }
    if ((control & 2u) && ++y->timer_b_sub >= 16u) {
        y->timer_b_sub = 0;
        if (++y->timer_b >= 256u) {
            y->timer_b = y->reg[0][0x26];
            if (control & 8u) y->status |= 2u;
        }
    }
}

static int clip9(int v) { return v > 256 ? 256 : v < -257 ? -257 : v; }

void md_ym_sample(MdYm2612 *y, int *left, int *right) {
    /* Key state is sampled once per sample: an off and on between two
     * samples changes nothing. */
    for (unsigned ch = 0; ch < 6; ++ch) ym_keys(y, ch);
    y->csm_key = 0;
    ym_timers(y);
    /* The envelope runs on every third sample; its counter counts those. */
    if ((++y->env_counter & 3u) == 3u) ++y->env_counter;
    const int envelope = (y->env_counter & 3u) == 0u;
    const uint32_t env_count = y->env_counter >> 2;
    /* LFO: 7-bit AM triangle and 5-bit signed PM from one counter. */
    unsigned lfo_am = 0x3f;   /* disabled: position 0, whose AM is 0x3f */
    int lfo_pm = 0;
    if (y->reg[0][0x22] & 8u) {
        const unsigned sub = y->lfo_counter++ & 0xffu;
        if (sub >= lfo_period[y->reg[0][0x22] & 7u]) y->lfo_counter += 0x101u - sub;
        lfo_am = (y->lfo_counter >> 8) & 0x3fu;
        if (!((y->lfo_counter >> 14) & 1u)) lfo_am ^= 0x3fu;
        lfo_pm = (int)((y->lfo_counter >> 10) & 7u);
        if ((y->lfo_counter >> 13) & 1u) lfo_pm ^= 7;
        if ((y->lfo_counter >> 14) & 1u) lfo_pm = -lfo_pm;
    } else y->lfo_counter = 0;
    *left = *right = 0;
    for (unsigned ch = 0; ch < 6; ++ch) {
        MdFmOperator *s1 = &y->op[ch][0];
        s1->prev = s1->out;           /* S1's last two outputs feed it back */
        s1->out = s1->next;
        const unsigned bank = ch / 3u, lane = ch % 3u;
        const uint8_t b0 = y->reg[bank][0xb0 + lane], b4 = y->reg[bank][0xb4 + lane];
        /* One phase base per block/F-num: the channel's, plus channel 3's
         * special-mode S1..S3. */
        const int sp3 = ch == 2 && (y->reg[0][0x27] & 0xc0u);
        unsigned base = phase_base(y, ch, (unsigned)y->block[ch] << 11 | y->fnum[ch], lfo_pm);
        const unsigned am = (lfo_am << 1) >> ((1u << (((b4 >> 4) & 3u) ^ 3u)) - 1u);
        int dead = 1;
        for (unsigned op = 0; op < 4; ++op)
            if (!op_silent(&y->op[ch][op])) { dead = 0; break; }
        for (unsigned op = 0; op < 4; ++op) {
            MdFmOperator *o = &y->op[ch][op];
            if (op_silent(o)) {
                /* Only the envelope keeps running, up to its 0x3ff floor. */
                if (envelope && o->att < EG_FLOOR) envelope_step(o, env_count);
                continue;
            }
            ssg_step(o);
            if (envelope) envelope_step(o, env_count);
            unsigned b = base;
            if (sp3 && special_fnum_of_op[op] >= 0)
                b = phase_base(y, ch, (unsigned)y->special_block[special_fnum_of_op[op]] << 11
                                     | y->special_fnum[special_fnum_of_op[op]], lfo_pm);
            phase_advance(o, b);
        }
        int out;
        if (ch == 5 && (y->reg[0][0x2b] & 0x80u)) {
            /* The DAC replaces channel 6, which is not computed meanwhile:
             * S1's feedback keeps its last value. */
            out = ((int)y->reg[0][0x2a] - 128) * 2;
        } else if (dead) {
            s1->next = 0;   /* every op_volume below would return 0 */
            out = 0;
        } else {
            const unsigned fb = (b0 >> 3) & 7u;
            const int a = op_volume(s1, fb ? (s1->out + s1->prev) >> (10u - fb) : 0, am);
            s1->next = (int16_t)a;
            MdFmOperator *const s2 = &y->op[ch][1], *const s3 = &y->op[ch][2], *const s4 = &y->op[ch][3];
            int b, c, d;
            switch (b0 & 7u) {
            case 0: b = op_volume(s2, a >> 1, am); c = op_volume(s3, b >> 1, am);
                    out = clip9(op_volume(s4, c >> 1, am) >> 5); break;
            case 1: b = op_volume(s2, 0, am); c = op_volume(s3, (a + b) >> 1, am);
                    out = clip9(op_volume(s4, c >> 1, am) >> 5); break;
            case 2: b = op_volume(s2, 0, am); c = op_volume(s3, b >> 1, am);
                    out = clip9(op_volume(s4, (a + c) >> 1, am) >> 5); break;
            case 3: b = op_volume(s2, a >> 1, am); c = op_volume(s3, 0, am);
                    out = clip9(op_volume(s4, (b + c) >> 1, am) >> 5); break;
            case 4: b = op_volume(s2, a >> 1, am); c = op_volume(s3, 0, am);
                    out = clip9((op_volume(s4, c >> 1, am) >> 5) + (b >> 5)); break;
            case 5: b = op_volume(s2, a >> 1, am); c = op_volume(s3, a >> 1, am);
                    d = op_volume(s4, a >> 1, am) >> 5;
                    out = clip9(clip9(d + (b >> 5)) + (c >> 5)); break;
            case 6: b = op_volume(s2, a >> 1, am); c = op_volume(s3, 0, am);
                    d = op_volume(s4, 0, am) >> 5;
                    out = clip9(clip9(d + (b >> 5)) + (c >> 5)); break;
            default: b = op_volume(s2, 0, am); c = op_volume(s3, 0, am);
                     d = op_volume(s4, 0, am) >> 5;
                     out = clip9(clip9(clip9(d + (a >> 5)) + (b >> 5)) + (c >> 5)); break;
            }
        }
        /* 9-bit DAC of the discrete YM2612 and its discontinuity around
         * zero (the "ladder effect"): a muted side still outputs it. */
        const int ladder = out < 0 ? out - 3 : out + 4;
        *left += ((b4 & 0x80u) ? ladder : 4) * 32;
        *right += ((b4 & 0x40u) ? ladder : 4) * 32;
    }
}
