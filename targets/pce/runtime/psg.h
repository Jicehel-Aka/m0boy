/* The PSG (the HuC6280's six wavetable channels): a 32-sample waveform a
 * channel, 12-bit frequency, a 5-bit volume, DDA mode (the last written
 * sample held) and the noise of a simple LFSR.  What the model leaves
 * out: the LFO (channels 1-2), the balance registers (games set them
 * symmetric) and the exact noise polynomial. */
#ifndef PCE_PSG_H
#define PCE_PSG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    uint8_t select;
    struct {
        uint16_t frequency;  /* 12 bits */
        uint8_t control;     /* 0x80 enable, 0x40 DDA, 0x1F volume */
        uint8_t noise;       /* bit 7 enable, bits 4-0 rate */
        uint8_t waveform[32];
        uint8_t waveform_index;
        uint8_t dda;         /* the held sample of the DDA mode */
        uint32_t acc;        /* the sample clock, 12 fractional bits */
    } ch[6];
    uint32_t lfsr;
} psg_t;

void psg_reset(psg_t *p);
void psg_write(psg_t *p, unsigned at, uint8_t value);
/* n samples of unsigned PCM at rate Hz, one frame's worth */
void psg_render(psg_t *p, uint8_t *dst, unsigned n, unsigned rate);

#ifdef __cplusplus
}
#endif
#endif
