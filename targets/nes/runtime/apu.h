/* The APU's four usable voices: two pulses (sweep, envelope, length), the
 * triangle (linear counter) and the noise (a 15-bit LFSR).  The DMC plays
 * nothing (its DMA would steal the bus; no game in scope depends on it).
 * $4015 enables; the frame-counter IRQ stays masked.  Sound renders a
 * frame at a time, nes_apu_render clocking every channel per sample. */
#ifndef NES_APU_H
#define NES_APU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    uint16_t timer[4];    /* the loaded period (noise: in CPU cycles too) */
    uint16_t counter[4];  /* the running period countdown */
    uint8_t length[4];    /* the length counters */
    uint8_t seq[4];       /* duty position (pulses), step (triangle) */
    uint8_t enable;
    uint8_t duty[2];
    uint8_t envelope[2], envelope_vol[2]; /* the pulses' decay */
    uint8_t envelope_const[2], envelope_halt[2];
    uint8_t envelope3_const, envelope3_vol, envelope3, envelope3_halt;
    uint8_t sweep_on[2], sweep_period[2], sweep_negate[2], sweep_shift[2], sweep_reload[2];
    uint8_t sweep_div[2];
    uint8_t triangle_halt, triangle_reload, triangle_linear;
    uint16_t lfsr;
    uint32_t env_div, len_div, sample_frac;
} nes_apu_t;

void nes_apu_reset(nes_apu_t *apu);
void nes_apu_write(nes_apu_t *apu, unsigned reg, uint8_t value); /* $4000-$4017 */
uint8_t nes_apu_read(nes_apu_t *apu);                            /* $4015 */
void nes_apu_render(nes_apu_t *apu, uint8_t *dst, unsigned n, unsigned rate);

#endif


#ifdef __cplusplus
}
#endif