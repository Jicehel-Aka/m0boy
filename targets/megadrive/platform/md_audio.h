/* Mega Drive audio for the ESP32 consoles: the 44.1 kHz stereo stream of
 * md_full_audio.c becomes the family's 11025 Hz mono 8-bit ring
 * (esp32_hw.cpp's ESP32_AUDIO_RENDER).  One frame is 127856*7 master clocks
 * at 53693175 Hz; the products exceed 32 bits, so they are computed in
 * 64 bits, once per rate. */
#ifndef M0_MD_AUDIO_H
#define M0_MD_AUDIO_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void md_platform_audio_rate(unsigned hz);
void md_platform_audio_render(uint8_t *out, unsigned n);

static inline unsigned md_audio_frame_samples(uint32_t rate, uint32_t *frac) {
    static uint32_t cached_rate, whole, part;
    if (rate != cached_rate) {
        const uint64_t per_frame = (uint64_t)rate * (127856u * 7u);
        cached_rate = rate;
        whole = (uint32_t)(per_frame / 53693175u);
        part = (uint32_t)(per_frame % 53693175u);
    }
    *frac += part;
    unsigned n = whole;
    if (*frac >= 53693175u) { *frac -= 53693175u; n++; }
    return n;
}

#ifdef __cplusplus
}
#endif
#endif
