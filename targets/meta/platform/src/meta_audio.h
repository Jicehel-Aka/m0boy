/* Audio output (META: DAC on PA02, meta_audio.cpp; Pokitto: R2R DAC, pokitto_audio.c). */
#ifndef META_AUDIO_H
#define META_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void meta_audio_init(void);
void meta_audio_frame(void);  /* once per emulated frame */
void meta_audio_idle(void);   /* called while waiting for frame pacing */
void meta_audio_set_volume(uint8_t v); /* 0..8, 4 = default */
uint8_t meta_audio_volume(void);
/* The output path is META_AUDIO_DMA, chosen at build time (0 the TC4
 * interrupt writes each ring entry to the DAC, 1 a DMAC beat copies it; the
 * META only, the Pokitto and ESPboy have their own drivers).  No run-time
 * switching since 0.10.29. */
void meta_audio_stop(void);   /* timer and interrupt off (before the loader) */
/* The option menu: the DAC to mid-scale and the sample source off, so the
 * game's sound actually stops while the menu is open (meta_audio_stop's
 * cheaper sibling: one call to pause, one to resume). */
void meta_audio_pause(int on);

/* Samples of one DMG frame (70224 cycles at 4194304 Hz) at `rate` Hz, the
 * remainder carried over in *frac (shared by the consoles' drivers). */
static inline unsigned meta_audio_frame_samples(uint32_t rate, uint32_t *frac) {
    *frac += rate * 70224u;
    unsigned n = *frac / 4194304u;
    *frac %= 4194304u;
    return n;
}

#ifdef __cplusplus
}
#endif

#endif
