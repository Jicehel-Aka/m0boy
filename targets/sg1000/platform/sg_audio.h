#pragma once
#include "sg1000.h"
#ifdef __cplusplus
extern "C" {
#endif
extern sg1000_t machine;
static inline void sg_platform_audio_rate(unsigned rate){sg1000_audio_rate(&machine,rate);}
static inline void sg_platform_audio_render(uint8_t *p,unsigned n){sg1000_audio_render(&machine,p,n);}
#ifdef __cplusplus
}
#endif
