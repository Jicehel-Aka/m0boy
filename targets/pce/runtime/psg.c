#include "psg.h"

#include <string.h>

/* the waveform clock runs off the 3.58 MHz half of the master clock: a
 * sample lasts (freq) of its cycles (mednafen's freq_cache = freq << 1
 * master clocks), so a channel advances 3579545 / freq waveform samples
 * a second */
#define PSG_CLOCK 3579545u

void psg_reset(psg_t *p) {
    memset(p, 0, sizeof *p);
    p->lfsr = 0x15555; /* not all zeroes, so the noise breathes */
}

void psg_write(psg_t *p, unsigned at, uint8_t value) {
    at &= 0x0F;
    if (at == 0x00) {
        p->select = value & 0x07;
        return;
    }
    if (p->select > 5) return;
    switch (at) {
    case 0x01: /* the global balance: ignored, games set it symmetric */
        break;
    case 0x02:
        p->ch[p->select].frequency = (uint16_t)((p->ch[p->select].frequency & 0x0F00) | value);
        break;
    case 0x03:
        p->ch[p->select].frequency = (uint16_t)((p->ch[p->select].frequency & 0x00FF) | ((value & 0x0F) << 8));
        break;
    case 0x04: /* enable, DDA, volume */
        if ((p->ch[p->select].control & 0x40) && !(value & 0x40))
            p->ch[p->select].waveform_index = 0;
        p->ch[p->select].control = value;
        break;
    case 0x05: /* the channel balance: ignored */
        break;
    case 0x06: /* the waveform, or the DDA stream */
        value &= 0x1F;
        if (!(p->ch[p->select].control & 0x40))
            p->ch[p->select].waveform[p->ch[p->select].waveform_index] = value;
        if ((p->ch[p->select].control & 0xC0) == 0x00)
            p->ch[p->select].waveform_index = (p->ch[p->select].waveform_index + 1) & 0x1F;
        if (p->ch[p->select].control & 0x80)
            p->ch[p->select].dda = value;
        break;
    case 0x07: /* the noise enable and rate */
        p->ch[p->select].noise = value;
        break;
    default: /* the LFO (0x08) and beyond: not modeled */
        break;
    }
}

/* the channels' sample clock, 12 fractional bits, per output sample */
static uint32_t wave_step(unsigned freq, unsigned rate) {
    if (!freq) freq = 4096;
    return (PSG_CLOCK << 12) / ((uint32_t)freq * rate);
}

static int channel_sample(psg_t *p, unsigned c, unsigned rate) {
    const uint8_t control = p->ch[c].control;
    if (!(control & 0x80)) return 0;
    if (p->ch[c].noise & 0x80) {
        /* the noise: an 18-bit LFSR (the tap pair is an approximation,
         * the polynomial of the chip is not published) */
        const unsigned nf = (p->ch[c].noise & 0x1F) + 1;
        p->ch[c].acc += (PSG_CLOCK << 12) / ((uint32_t)nf * rate);
        while (p->ch[c].acc >> 12) {
            p->ch[c].acc -= 1u << 12;
            const unsigned fb = ((p->lfsr >> 17) ^ (p->lfsr >> 12)) & 1;
            p->lfsr = ((p->lfsr << 1) | fb) & 0x3FFFF;
        }
        return (p->lfsr >> 17) & 1 ? 15 : -16;
    }
    if (control & 0x40) /* DDA: the last written sample, held */
        return (int)p->ch[c].dda - 16;
    p->ch[c].acc += wave_step(p->ch[c].frequency, rate);
    if (p->ch[c].acc >> 12) {
        p->ch[c].waveform_index = (p->ch[c].waveform_index + (p->ch[c].acc >> 12)) & 0x1F;
        p->ch[c].acc &= 0x0FFF;
    }
    return (int)p->ch[c].waveform[p->ch[c].waveform_index] - 16;
}

void psg_render(psg_t *p, uint8_t *dst, unsigned n, unsigned rate) {
    for (unsigned i = 0; i < n; i++) {
        int mix = 0;
        for (unsigned c = 0; c < 6; c++) {
            const int s = channel_sample(p, c, rate);
            if (s) mix += s * (p->ch[c].control & 0x1F);
        }
        /* 6 channels x 31 x 15, signed: scale into the unsigned 8 bits */
        mix = mix / 24 + 128;
        dst[i] = (uint8_t)(mix < 0 ? 0 : mix > 255 ? 255 : mix);
    }
}
