/* Audio output for the Gamebuino META (DAC on PA02). */
#ifndef META_AUDIO_H
#define META_AUDIO_H

void meta_audio_init(void);
void meta_audio_frame(void);  /* once per emulated frame */
void meta_audio_idle(void);   /* called while waiting for frame pacing */
void meta_audio_stop(void);   /* timer and interrupt off (before the loader) */

#endif
