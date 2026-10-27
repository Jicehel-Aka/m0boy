#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void md_platform_audio_rate(unsigned hz);
void md_platform_audio_render(uint8_t *out, unsigned n);
static inline unsigned md_audio_frame_samples(uint32_t rate, uint32_t *frac) {
    *frac += rate * 10000u;
    unsigned count = *frac / 597275u;
    *frac %= 597275u;
    return count;
}
#ifdef __cplusplus
}
#endif
