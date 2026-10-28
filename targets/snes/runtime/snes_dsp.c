/* The S-DSP: eight BRR voices with the ADSR/GAIN envelopes, the pitch
 * counters and the volumes, rendering stereo into a ring the host drains
 * at 32000 Hz (one sample per 1024 SPC cycles, from snes_smp.c's clock).
 * The register file is what the SPC700 sees at $F2/$F3.
 *
 * Deliberately not modelled: the echo (its RAM area, feedback, filters,
 * the EDL delay), the noise source, pitch modulation (PMON), the 4-point
 * gaussian interpolation (the voices read their nearest sample), the
 * sample-accurate ADSR step tables (the envelopes follow the shapes with
 * the documented period table), the soft reset's exact clears, and the
 * EXTL/EXTR inputs.  The mute and reset bits of $6C are honoured. */
#include "snes.h"

#include <string.h>

#define ADSR_MAX 0x7FF

/* the envelope period table: the rates' samples per step, 0 = never */
static const uint16_t rate_period[32] = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512,
    384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12,
    10, 8, 6, 5, 4, 3, 2, 1
};

/* the register file's voice base */
#define VREG(v, r) machine.dsp.regs[(v) * 0x10 + (r)]
#define GREG(r) machine.dsp.regs[r]

void snes_dsp_write(uint8_t at, uint8_t v) {
    snes_dsp_t *d = &machine.dsp;
    d->regs[at & 0x7F] = v;
    switch (at & 0x7F) {
    case 0x4C: d->key_on |= v; break;  /* KON: the voices restart at the next sample */
    case 0x5C: d->key_off = v; break;  /* KOF: the voices release */
    default: break;
    }
}

uint8_t snes_dsp_read(uint8_t at) {
    snes_dsp_t *d = &machine.dsp;
    at &= 0x7F;
    if ((at & 0xF) == 0x8) { /* ENVX: the voice's current envelope */
        const unsigned v = at >> 4;
        return (uint8_t)(d->voice[v].env >> 4);
    }
    if ((at & 0xF) == 0x9) /* ENDX: the block-end flag of the voice */
        return (uint8_t)(((d->end_flags >> (at >> 4)) & 1) << 7);
    return d->regs[at];
}

/* the voice's sample source: the directory entry's two block addresses */
static void voice_load_source(unsigned v) {
    const unsigned dir = GREG(0x5D) * 0x100u;
    const unsigned src = VREG(v, 4);
    const uint16_t start = (uint16_t)(machine.smp.ram[(dir + src * 4) & 0xFFFF] |
                                      machine.smp.ram[(dir + src * 4 + 1) & 0xFFFF] << 8);
    const uint16_t loop = (uint16_t)(machine.smp.ram[(dir + src * 4 + 2) & 0xFFFF] |
                                     machine.smp.ram[(dir + src * 4 + 3) & 0xFFFF] << 8);
    machine.dsp.voice[v].start = (uint16_t)(start * 8u);
    machine.dsp.voice[v].loop = (uint16_t)(loop * 8u);
}

/* start a voice: the BRR reader at its start block, the envelope at the
 * attack or the direct gain */
static void voice_start(unsigned v) {
    snes_dsp_t *d = &machine.dsp;
    voice_load_source(v);
    d->voice[v].src = d->voice[v].start;
    d->voice[v].hdr = machine.smp.ram[d->voice[v].src];
    d->voice[v].nib = 0;
    d->voice[v].pos = 0;
    d->voice[v].prev1 = d->voice[v].prev2 = 0;
    if (VREG(v, 5) & 0x80) {
        d->voice[v].adsr_state = 1; /* attack */
        d->voice[v].env = 0;
    } else {
        d->voice[v].adsr_state = 0; /* the gain's direct value */
        d->voice[v].env = (VREG(v, 7) & 0x80) ? 0 : (VREG(v, 7) & 0x7F) << 4;
    }
    d->voice[v].rate_count = 0;
}

/* one BRR sample out of the current block, the filters applied; ends the
 * voice or loops when the block's flags say so */
static int16_t voice_next_sample(unsigned v) {
    snes_dsp_t *d = &machine.dsp;
    const uint8_t hdr = d->voice[v].hdr;
    const unsigned shift = hdr >> 4;
    const unsigned filter = (hdr >> 2) & 3;
    const uint8_t byte = machine.smp.ram[(d->voice[v].src + 1 + d->voice[v].nib / 2) & 0xFFFF];
    const int s = (d->voice[v].nib & 1 ? byte & 0xF : byte >> 4);
    int sample = (s & 8 ? s - 16 : s) << shift;
    sample <<= 4; sample >>= 4; /* sign-extend through the shift */
    const int16_t s1 = d->voice[v].prev1, s2 = d->voice[v].prev2;
    int out;
    switch (filter) {
    case 1: out = sample + ((s1 * 15) >> 4); break;
    case 2: out = sample + (((s1 * 61) >> 5) - ((s2 * 15) >> 4)); break;
    case 3: out = sample + (((s1 * 115) >> 6) - ((s2 * 13) >> 4)); break;
    default: out = sample; break;
    }
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    d->voice[v].prev2 = s1;
    d->voice[v].prev1 = (int16_t)out;
    d->voice[v].nib++;
    if (d->voice[v].nib == 16) { /* the block is through: its flags decide */
        d->voice[v].nib = 0;
        if (hdr & 1) d->end_flags |= 1u << v;
        if (hdr & 2) d->voice[v].src = d->voice[v].loop;
        else if (hdr & 1) {
            d->voice[v].adsr_state = 4; /* end without loop: release */
            d->voice[v].rate_count = 0;
            return (int16_t)out;
        }
        d->voice[v].src = (uint16_t)(d->voice[v].src + 9);
        d->voice[v].hdr = machine.smp.ram[d->voice[v].src];
    }
    return (int16_t)out;
}

/* the envelope's one step, by the state and the register fields */
static void voice_envelope(unsigned v) {
    snes_dsp_t *d = &machine.dsp;
    d->voice[v].rate_count++;
    switch (d->voice[v].adsr_state) {
    case 1: { /* attack: linear to the top */
        const unsigned period = rate_period[(VREG(v, 5) & 0xF) * 2 + 1];
        if (!period) return;
        if (d->voice[v].rate_count < period) return;
        d->voice[v].rate_count = 0;
        d->voice[v].env = (int16_t)((d->voice[v].env + 1) & 0x7FF);
        if (d->voice[v].env >= ADSR_MAX) d->voice[v].adsr_state = 2;
        break;
    }
    case 2: { /* decay: -8 per period to the sustain level */
        const unsigned period = rate_period[((VREG(v, 5) >> 4) & 7) * 2 + 16];
        if (!period) return;
        if (d->voice[v].rate_count < period) return;
        d->voice[v].rate_count = 0;
        const int level = (((VREG(v, 6) >> 5) & 7) + 1) << 8;
        d->voice[v].env = (int16_t)(d->voice[v].env - 8);
        if (d->voice[v].env <= level) {
            d->voice[v].env = (int16_t)(level > ADSR_MAX ? ADSR_MAX : level);
            d->voice[v].adsr_state = 3;
        }
        break;
    }
    case 3: break; /* sustain: the level holds */
    case 4: { /* release: -8 per period to silence */
        const unsigned period = rate_period[VREG(v, 6) & 0x1F];
        if (!period) return;
        if (d->voice[v].rate_count < period) return;
        d->voice[v].rate_count = 0;
        d->voice[v].env = (int16_t)(d->voice[v].env - 8);
        if (d->voice[v].env <= 0) d->voice[v].env = 0;
        break;
    }
    default: { /* the gain modes */
        const uint8_t g = VREG(v, 7);
        if (!(g & 0x80)) {
            d->voice[v].env = (int16_t)((g & 0x7F) << 4);
            return;
        }
        const unsigned period = rate_period[g & 0x1F];
        if (!period) return;
        if (d->voice[v].rate_count < period) return;
        d->voice[v].rate_count = 0;
        switch ((g >> 5) & 3) {
        case 0: d->voice[v].env = (int16_t)(d->voice[v].env - 1); break;    /* linear dec */
        case 1: d->voice[v].env = (int16_t)(d->voice[v].env + 1); break;    /* linear inc */
        case 2: d->voice[v].env = (int16_t)(d->voice[v].env - 8); break;    /* bent dec */
        case 3: d->voice[v].env = (int16_t)(d->voice[v].env - (d->voice[v].env >> 8) - 1); break;
        }
        if (d->voice[v].env < 0) d->voice[v].env = 0;
        if (d->voice[v].env > ADSR_MAX) d->voice[v].env = ADSR_MAX;
        break;
    }
    }
}

/* one output sample: the KON/KOF latches, the voices, the mix, the ring */
void snes_dsp_sample(void) {
    snes_dsp_t *d = &machine.dsp;
    if (d->key_on) {
        for (unsigned v = 0; v < 8; v++)
            if (d->key_on & (1u << v)) voice_start(v);
        d->key_on = 0;
    }
    if (d->key_off) {
        for (unsigned v = 0; v < 8; v++)
            if (d->key_off & (1u << v)) {
                d->voice[v].adsr_state = 4;
                d->voice[v].rate_count = 0;
            }
        d->key_off = 0;
    }
    int32_t mix_l = 0, mix_r = 0;
    for (unsigned v = 0; v < 8; v++) {
        voice_envelope(v);
        const unsigned pitch = (VREG(v, 3) << 8 | VREG(v, 2)) & 0x3FFF;
        if (!pitch || !d->voice[v].env) continue;
        d->voice[v].pos += pitch;
        while (d->voice[v].pos >= 0x1000) { /* the next BRR sample */
            d->voice[v].pos -= 0x1000;
            d->voice[v].out = voice_next_sample(v);
        }
        const int32_t s = ((int32_t)d->voice[v].out * d->voice[v].env) >> 11;
        const int8_t vl = (int8_t)VREG(v, 0), vr = (int8_t)VREG(v, 1);
        mix_l += (s * vl) >> 7;
        mix_r += (s * vr) >> 7;
    }
    const uint8_t flg = GREG(0x6C);
    if (flg & 0x20) { /* the soft reset: the voices quiet until cleared */
        mix_l = mix_r = 0;
        for (unsigned v = 0; v < 8; v++) d->voice[v].env = 0;
    }
    if (flg & 0x10) { mix_l = mix_r = 0; } /* mute */
    const int8_t ml = (int8_t)GREG(0x0C), mr = (int8_t)GREG(0x1C);
    int32_t l = (mix_l * ml) >> 7, r = (mix_r * mr) >> 7;
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    d->ring[d->ring_w] = (int16_t)l;
    d->ring[(d->ring_w + 1) & 8191] = (int16_t)r;
    d->ring_w = (uint16_t)((d->ring_w + 2) & 8191);
}
