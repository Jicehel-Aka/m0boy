/* PICO-8 sound: 4 channels of sfx and the music sequencer, mixed into 8-bit
 * samples (128 = silence).  Integer only: no floating point on the console.
 * The structure follows femto8's p8_audio.c; everything runs in the context
 * of the main loop (sfx() and music() take effect at once, the mixer is
 * called once a frame), so there is no queue. */
#ifndef P8_AUDIO_H
#define P8_AUDIO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void p8_audio_init(unsigned rate);          /* stop everything */
void p8_audio_set_rate(unsigned rate);      /* the sample rate of the driver */
void p8_audio_render(uint8_t *buffer, unsigned n);
/* sfx(n, channel, start, end): n = -1 stops the channel (-2: every channel
 * playing n); channel -1: a free one */
void p8_audio_sfx(int n, int channel, int start, int end);
void p8_audio_music(int pattern, int fade_ms, int mask);
int p8_audio_stat(int index);
#ifdef __cplusplus
}
#endif
#endif
