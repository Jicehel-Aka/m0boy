#pragma once
#include "gg.h"
#ifdef __cplusplus
extern "C" {
#endif
static inline void sg_platform_audio_rate(unsigned rate) { gg_audio_rate(rate); }
static inline void sg_platform_audio_render(uint8_t *p, unsigned n) { gg_audio_render(p, n); }
#ifdef __cplusplus
}
#endif
