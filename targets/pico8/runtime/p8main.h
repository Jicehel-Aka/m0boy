/* Running a recompiled cartridge: one frame at a time, so the host harness
 * and the console frontends drive the same loop. */
#ifndef P8MAIN_H
#define P8MAIN_H
#include "p8rt.h"
#include "p8gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

extern Val *const p8_cb_init, *const p8_cb_update, *const p8_cb_update60, *const p8_cb_draw;
void p8_gen_run_main(void);
extern unsigned p8_audio_rate_hint; /* sample rate of the audio driver (22050 by default) */

/* heap, drawing state, input, random numbers, main chunk and _init */
void p8_start(void);
/* _update, _draw, then the buttons held now (bit 0 left ... bit 5 X) */
void p8_frame(uint16_t buttons);
#ifdef __cplusplus
}
#endif

#endif
